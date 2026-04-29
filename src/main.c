/* --------------------------------------------------------------
   Application: 06 
   Theme Park Ride Safety Controller
   Class: Real Time Systems - Spring 2026
   Name: Colby Tuttle

   Description:
   This is a theme park ride safety control system using an esp32 and FreeRTOS.
   It monitors a simulated ride, responds to a hard deadline using an emergency stop
   button, uses status leds, and logs timing information as well as periodically runs a diagnostics task.
---------------------------------------------------------------*/

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "driver/gpio.h"
#include "driver/adc.h"
#include "esp_timer.h"
#include "esp_log.h"

/* pin connections
   GPIO2  : green heartbeat led  -> ride controller alive
   GPIO18 : yellow warning led   -> soft warning / sensor threshold
   GPIO19 : red emergency led    -> hard emergency stop condition
   GPIO4  : momentary pushbutton -> emergency stop input, active low
   GPIO32 : potentiometer wiper  -> simulated ride speed/load sensor */
   
//ai used to confirm my connections were fully functional
#define HEARTBEAT_LED_GPIO GPIO_NUM_2
#define WARNING_LED_GPIO GPIO_NUM_18
#define EMERGENCY_LED_GPIO GPIO_NUM_19
#define ESTOP_BUTTON_GPIO GPIO_NUM_4
#define SENSOR_ADC_CHANNEL ADC1_CHANNEL_4

//timing definitions for requirements
#define SENSOR_PERIOD_MS 200
#define EMERGENCY_DEADLINE_MS 50
#define HEARTBEAT_PERIOD_MS 250
#define LOGGER_PERIOD_MS 2000
#define DIAGNOSTIC_PERIOD_MS 3000

#define SENSOR_WARNING_THRESHOLD 2600
#define SENSOR_ESTOP_THRESHOLD 3600
#define SENSOR_QUEUE_LENGTH 8

static const char *TAG = "RIDE_RTS";

//struct for ride state machine
typedef enum
{
    RIDE_STATE_NORMAL = 0,
    RIDE_STATE_WARNING,
    RIDE_STATE_ESTOP
} ride_state_t;

//struct for message passed to logger
typedef struct
{
    int raw_adc;
    int64_t timestamp_ms;
    bool warning;
    bool emergency;
} sensor_msg_t;

//synchronization primitives
static QueueHandle_t sensorQueue = NULL;
static SemaphoreHandle_t estopSemaphore = NULL;
static SemaphoreHandle_t serialMutex = NULL;
static portMUX_TYPE rideStateMux = portMUX_INITIALIZER_UNLOCKED;

static volatile int64_t lastEstopIsrTimeMs = 0;
static ride_state_t rideState = RIDE_STATE_NORMAL;

//simple helper for time in ms
static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

//converting enum to string
static const char *ride_state_to_string(ride_state_t state)
{
    switch (state)
    {
        case RIDE_STATE_NORMAL: return "NORMAL";
        case RIDE_STATE_WARNING: return "WARNING";
        case RIDE_STATE_ESTOP: return "ESTOP";
        default: return "UNKNOWN";
    }
}

//mutex for logging
static void safe_log(const char *message)
{
    if (xSemaphoreTake(serialMutex, pdMS_TO_TICKS(20)) == pdTRUE)
    {
        printf("%s", message);
        xSemaphoreGive(serialMutex);
    }
}

static ride_state_t get_ride_state(void)
{
    ride_state_t state;
    //ai used for critical section instead of mutex for faster access
    portENTER_CRITICAL(&rideStateMux);
    state = rideState;
    portEXIT_CRITICAL(&rideStateMux);
    return state;
}

static void set_ride_state(ride_state_t newState)
{
    //race proofing
    //similar logic as above
    portENTER_CRITICAL(&rideStateMux);
    rideState = newState;
    portEXIT_CRITICAL(&rideStateMux);
}

//ISR for estop button
static void IRAM_ATTR estop_button_isr_handler(void *arg)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    //records time of interrupt
    lastEstopIsrTimeMs = now_ms();
    //using safe ISR function
    xSemaphoreGiveFromISR(estopSemaphore, &xHigherPriorityTaskWoken);
    //context switch if higher priority task appears
    if (xHigherPriorityTaskWoken == pdTRUE)
    {
        portYIELD_FROM_ISR();
    }
}

//sensor task to monitor ride condition
static void sensor_monitor_task(void *pvParameters)
{
    TickType_t lastWakeTime = xTaskGetTickCount();
    while (true)
    {
        int64_t startMs = now_ms();
        //read sensor value
        int raw = adc1_get_raw(SENSOR_ADC_CHANNEL);

        sensor_msg_t msg =
        {
            .raw_adc = raw,
            .timestamp_ms = startMs,
            .warning = (raw >= SENSOR_WARNING_THRESHOLD),
            .emergency = (raw >= SENSOR_ESTOP_THRESHOLD)
        };

        //logic to update state based on sensor input
        //ai used to help with correct logic
        if (msg.emergency && get_ride_state() != RIDE_STATE_ESTOP)
        {
            lastEstopIsrTimeMs = startMs;
            set_ride_state(RIDE_STATE_ESTOP);
            xSemaphoreGive(estopSemaphore);
        }
        else if (msg.warning && get_ride_state() != RIDE_STATE_ESTOP)
        {
            set_ride_state(RIDE_STATE_WARNING);
        }
        else if (!msg.warning && get_ride_state() != RIDE_STATE_ESTOP)
        {
            set_ride_state(RIDE_STATE_NORMAL);
        }

        if (xQueueSend(sensorQueue, &msg, 0) != pdTRUE)
        {
            safe_log("[SENSOR] queue full; sample dropped.\n");
        }

        int64_t finishMs = now_ms();

        if ((finishMs - startMs) > SENSOR_PERIOD_MS)
        {
            safe_log("[MISS] HARD sensor task exceeded deadline.\n");
        }

        vTaskDelayUntil(&lastWakeTime, pdMS_TO_TICKS(SENSOR_PERIOD_MS));
    }
}

//handling estop event
static void emergency_response_task(void *pvParameters)
{
    while (true)
    {
        if (xSemaphoreTake(estopSemaphore, portMAX_DELAY) == pdTRUE)
        {
            int64_t responseMs = now_ms();

            //latency calculation for deadline verification
            int64_t latencyMs = responseMs - lastEstopIsrTimeMs;

            //force estop state
            set_ride_state(RIDE_STATE_ESTOP);
            //emergency indicators
            gpio_set_level(EMERGENCY_LED_GPIO, 1);
            gpio_set_level(WARNING_LED_GPIO, 0);

            char buffer[160];
            snprintf(buffer, sizeof(buffer), "[ESTOP] handled @ %lld ms | latency = %lld ms | deadline = %d ms\n", responseMs, latencyMs, EMERGENCY_DEADLINE_MS);
            safe_log(buffer);

            //hard deadline validation
            if (latencyMs > EMERGENCY_DEADLINE_MS)
            {
                safe_log("[MISS] HARD emergency response exceeded deadline.\n");
            }
        }
    }
}

//handle heartbeat task/active ride
static void heartbeat_task(void *pvParameters)
{
    TickType_t lastWakeTime = xTaskGetTickCount();
    int ledState = 0;

    while (true)
    {
        //on while not in estop state
        if (get_ride_state() != RIDE_STATE_ESTOP)
        {
            ledState = !ledState;
            gpio_set_level(HEARTBEAT_LED_GPIO, ledState);
        }
        else
        {
            gpio_set_level(HEARTBEAT_LED_GPIO, 0);
        }

        vTaskDelayUntil(&lastWakeTime, pdMS_TO_TICKS(HEARTBEAT_PERIOD_MS));
    }
}

//printing system status
static void logger_task(void *pvParameters)
{
    sensor_msg_t msg;

    while (true)
    {
        if (xQueueReceive(sensorQueue, &msg, pdMS_TO_TICKS(LOGGER_PERIOD_MS)) == pdTRUE)
        {
            ride_state_t state = get_ride_state();
            //update warning LED per state
            gpio_set_level(WARNING_LED_GPIO, (state == RIDE_STATE_WARNING));

            char buffer[180];
            snprintf(buffer, sizeof(buffer), "[LOG] t=%lld | sensor=%d | state=%s\n", msg.timestamp_ms, msg.raw_adc, ride_state_to_string(state));
            safe_log(buffer);
        }
    }
}

//diagnostic task for background workload
static void diagnostic_task(void *pvParameters)
{
    TickType_t lastWakeTime = xTaskGetTickCount();
    uint32_t scanCount = 0;

    while (true)
    {
        int64_t startMs = now_ms();
        int raw = adc1_get_raw(SENSOR_ADC_CHANNEL);

        //ai used to help with diagnostic variability
        int loops = 2000 + (raw / 8);
        volatile uint32_t checksum = 0;

        for (int i = 0; i < loops; i++)
        {
          //preventing optimization (ai help here as well)
          checksum += (i ^ raw) & 0xFF;
        }

        int64_t finishMs = now_ms();
        scanCount++;
        char buffer[180];
        snprintf(buffer, sizeof(buffer), "[DIAG] scan=%lu | runtime=%lld ms | SOFT task\n", (unsigned long)scanCount, finishMs - startMs);
        safe_log(buffer);

        vTaskDelayUntil(&lastWakeTime, pdMS_TO_TICKS(DIAGNOSTIC_PERIOD_MS));
    }
}


//GPIO and ADC setup
static void configure_hardware(void)
{
    gpio_config_t outputConfig =
    {
        .pin_bit_mask = (1ULL << HEARTBEAT_LED_GPIO) | (1ULL << WARNING_LED_GPIO) | (1ULL << EMERGENCY_LED_GPIO),
        .mode = GPIO_MODE_OUTPUT
    };
    gpio_config(&outputConfig);

    gpio_config_t buttonConfig =
    {
        .pin_bit_mask = (1ULL << ESTOP_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_NEGEDGE
    };
    gpio_config(&buttonConfig);
    //adc config
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(SENSOR_ADC_CHANNEL, ADC_ATTEN_DB_11);
    //isr installment
    gpio_install_isr_service(0);
    gpio_isr_handler_add(ESTOP_BUTTON_GPIO, estop_button_isr_handler, NULL);
}

void app_main(void)
{
  //call for hardware setup
  configure_hardware();

  sensorQueue = xQueueCreate(SENSOR_QUEUE_LENGTH, sizeof(sensor_msg_t));
  estopSemaphore = xSemaphoreCreateBinary();
  serialMutex = xSemaphoreCreateMutex();

  if (sensorQueue == NULL || estopSemaphore == NULL || serialMutex == NULL)
  {
    ESP_LOGE(TAG, "failed to create rtos primitives");
    return;
  }

  safe_log("system started\n");
  //priority design
  xTaskCreatePinnedToCore(emergency_response_task, "Emergency", 4096, NULL, 5, NULL, 1);
  xTaskCreatePinnedToCore(sensor_monitor_task, "Sensor", 4096, NULL, 4, NULL, 1);
  xTaskCreatePinnedToCore(heartbeat_task, "Heartbeat", 2048, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(logger_task, "Logger", 4096, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(diagnostic_task, "Diagnostic", 4096, NULL, 1, NULL, 1);
}
