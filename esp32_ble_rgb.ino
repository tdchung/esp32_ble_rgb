/**
 * 
 * eps32 BLE serial - led control
 * tdchung.9@gmail.com
 **/

#define BLE 1
#define BLUETOOTH_CLASSIC 0

#if BLUETOOTH_CLASSIC
#include "BluetoothSerial.h"
#endif

#if BLE
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#endif

#if !defined(CONFIG_BT_ENABLED) || !defined(CONFIG_BLUEDROID_ENABLED)
#error Bluetooth is not enabled! Please run `make menuconfig` to and enable it
#endif

#if CONFIG_FREERTOS_UNICORE
#define ARDUINO_RUNNING_CORE 0
#else
#define ARDUINO_RUNNING_CORE 1
#endif

// set debug mode
#define DEBUG_MODE 1

// define ANOT
#define ANOT 1

// configuration
// led pins
#define PIN_RED 12
#define PIN_GREEN 15
#define PIN_BLUE 14

// PWM setting
#define PWM_PREQ 5000
#define CHANNEL_RED 0
#define CHANNEL_GREEN 1
#define CHANNEL_BLUE 2
#define RESOLUTION 8

// delay setting
#define FADE_DELAY 15 // 40 ms
#define FLASH_DELAY 50

#ifndef LED_BUILTIN
#define LED_BUILTIN 13
#endif

// BLE message
#define END_OF_DATA_CHAR '\r'
#define MAX_BLE_MSG_LENGTH 128

typedef enum
{
    NONE = 0,
    FLASH,
    FADE,
    DIMMER,
    COLOR_COLLECTION,
    TIMER,
    IN_TIMER,
    OFF
} led_mode_t;

// mode tracker
volatile led_mode_t led_mode = NONE;
// bool isConnected = false;

// r,g,b color
uint8_t r = 255;
uint8_t g = 255;
uint8_t b = 255;

uint8_t dimming_percent = 0;

uint8_t timer = 0;
volatile bool isTimer = false;
volatile bool isTimerReset = false;

#if BLUETOOTH_CLASSIC
BluetoothSerial SerialBT;
#endif

#if BLE
BLEServer *pServer = NULL;
BLECharacteristic *pCharacteristic = NULL;

#define SERVICE_UUID "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#endif

#if DEBUG_MODE
SemaphoreHandle_t mutex_v;
#endif

SemaphoreHandle_t mutex_led_mode;
SemaphoreHandle_t mutex_color;

/*------------------------safe--------------------------*/
led_mode_t get_led_mode()
{
    led_mode_t temp;
    xSemaphoreTake(mutex_led_mode, portMAX_DELAY);
    temp = led_mode;
    xSemaphoreGive(mutex_led_mode);
    return temp;
}

void set_led_mode(led_mode_t mode)
{
    xSemaphoreTake(mutex_led_mode, portMAX_DELAY);
    led_mode = mode;
    xSemaphoreGive(mutex_led_mode);
    return;
}

int debug(const char *fmt, ...)
{
    int rc = 0;
#if DEBUG_MODE
    char buffer[4096];
    va_list args;
    va_start(args, fmt);
    rc = vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    xSemaphoreTake(mutex_v, portMAX_DELAY);
    Serial.println(buffer);
    xSemaphoreGive(mutex_v);
#endif
    return rc;
}

static uint8_t _biggest(uint8_t x, uint8_t y, uint8_t z)
{
    return x > y ? (x > z ? 1 : 3) : (y > z ? 2 : 3);
}

static void set_color(uint8_t rr, uint8_t gg, uint8_t bb)
{
#ifdef ANOT
    // set PWN dutyCycle
    ledcWrite(CHANNEL_RED, 255 - rr);
    ledcWrite(CHANNEL_GREEN, 255 - gg);
    ledcWrite(CHANNEL_BLUE, 255 - bb);
#else
    // set PWN dutyCycle
    ledcWrite(CHANNEL_RED, rr);
    ledcWrite(CHANNEL_GREEN, gg);
    ledcWrite(CHANNEL_BLUE, bb);
#endif
    debug("COLOR: %d, %d, %d", rr, gg, bb);
}

/*--------------------------COLOR MODE---------------------------*/
void color_collection(void)
{
    xSemaphoreTake(mutex_color, portMAX_DELAY);
    set_color(r, g, b);
    xSemaphoreGive(mutex_color);
    // reset led mode
    set_led_mode(NONE);
}

void color_off(void)
{
    set_color(0, 0, 0);
    // reset led mode
    set_led_mode(NONE);
}

void fading_led(void)
{
    uint8_t index = _biggest(r, g, b);
    uint8_t max_loop = ((1 == index) ? r : (2 == index) ? g : b);

    uint8_t r_temp, g_temp, b_temp;
    xSemaphoreTake(mutex_color, portMAX_DELAY);
    r_temp = r;
    g_temp = g;
    b_temp = b;
    xSemaphoreGive(mutex_color);
    //
    for (uint8_t i = 0; i < max_loop; i++)
    {
        r_temp = ((0 >= r_temp) ? 0 : r_temp - 1);
        g_temp = ((0 >= g_temp) ? 0 : g_temp - 1);
        b_temp = ((0 >= b_temp) ? 0 : b_temp - 1);
        set_color(r_temp, g_temp, b_temp);

        vTaskDelay(FADE_DELAY / portTICK_PERIOD_MS);
        // stop if mode changed
        if (FADE != get_led_mode())
            return;
    }

    // reset mode when it done
    set_led_mode(NONE);
}

void flash_led(void)
{
    if (FLASH == get_led_mode())
    {
        xSemaphoreTake(mutex_color, portMAX_DELAY);
        set_color(r, g, b);
        xSemaphoreGive(mutex_color);

        vTaskDelay(FLASH_DELAY / portTICK_PERIOD_MS);
        set_color(0, 0, 0);
        vTaskDelay(FLASH_DELAY / portTICK_PERIOD_MS);
    }
    // stop if mode changed
}

void dimming_led(void)
{
    xSemaphoreTake(mutex_color, portMAX_DELAY);
    set_color((uint8_t)r * dimming_percent / 100, (uint8_t)g * dimming_percent / 100, (uint8_t)b * dimming_percent / 100);
    xSemaphoreGive(mutex_color);
    // reset MODE
    set_led_mode(NONE);
    debug("Dimmer, reset Led mode: %d", (uint8_t)get_led_mode());
}

void timer_led(void)
{
    // set_led_mode(IN_TIMER);
    // xSemaphoreTake(mutex_color, portMAX_DELAY);
    // set_color(r, g, b);
    // xSemaphoreGive(mutex_color);

    // for (uint8_t i = 0; i < timer * 10; i++)
    // {
    //     if (IN_TIMER == get_led_mode())
    //     {
    //         vTaskDelay(100 / portTICK_PERIOD_MS);
    //         if ((0 != i) && (0 == (i % 10)))
    //             debug("Timer expried in: %d (s)", (uint8_t)(timer - i / 10));
    //     }
    //     else
    //         return;
    // }
    // // reset MODE
    // if (IN_TIMER == get_led_mode())
    // {
    //     set_color(0, 0, 0);
    //     set_led_mode(NONE);
    //     debug("Timer, reset Led mode: %d", (uint8_t)get_led_mode());
    // }
}

/*--------------------------Parse MSG---------------------------*/
void parse_lte_msg(const char *msg)
{
    // char temp[MAX_BLE_MSG_LENGTH + 1] = {0};
#ifdef __cplusplus
    char *temp = (char *)malloc(MAX_BLE_MSG_LENGTH);
#else
    char *temp = malloc(MAX_BLE_MSG_LENGTH);
#endif
    memset(temp, 0, MAX_BLE_MSG_LENGTH);
    strncpy(temp, msg, MAX_BLE_MSG_LENGTH);

    if (NULL != strstr(temp, "flash"))
    {
        set_led_mode(FLASH);
    }
    else if (NULL != strstr(temp, "fade"))
    {
        set_led_mode(FADE);
    }
    else if (NULL != strstr(temp, "dimmer"))
    {
        set_led_mode(DIMMER);
        // FORMAT: dimmer 0 .
        char str_temp[20];
        uint8_t percent_temp;
        if (2 == sscanf(strstr(temp, "dimmer"), "%s %d", str_temp, &percent_temp))
        {
            dimming_percent = percent_temp;
            debug("Parse message, dimmer percent: %d", dimming_percent);
        }
        else
            set_led_mode(NONE);
    }
    else if (NULL != strstr(temp, "color"))
    {
        set_led_mode(COLOR_COLLECTION);
        // FORMAT: color 100 50 222 .
        char str_temp[20];
        uint8_t r_temp, g_temp, b_temp;
        if (4 == sscanf(strstr(temp, "color"), "%s %d %d %d", str_temp, &r_temp, &g_temp, &b_temp))
        {
            xSemaphoreTake(mutex_color, portMAX_DELAY);
            r = r_temp;
            g = g_temp;
            b = b_temp;
            xSemaphoreGive(mutex_color);

            debug("Parse message, color: %d %d %d", r_temp, g_temp, b_temp);
        }
        else
            set_led_mode(NONE);
    }
    else if (NULL != strstr(temp, "timer"))
    {
        // FORMAT: timer <second> .
        // set_led_mode(TIMER);
        char str_temp[20];
        uint8_t timer_p;
        if (2 == sscanf(strstr(temp, "timer"), "%s %d", str_temp, &timer_p))
        {
            timer = timer_p;
            debug("Parse message, timer (s): %d", timer);
            isTimer = true;
            isTimerReset = true;
        }
        else
            set_led_mode(NONE);
    }
    else if (NULL != strstr(temp, "off"))
    {
        set_led_mode(OFF);
    }
    else
        ;
    // not change led mode
    // led_mode = NONE;

    debug("Parse message, Led mode: %d", (uint8_t)get_led_mode());

    // send ACK
#if BLUETOOTH_CLASSIC
    SerialBT.write((const uint8_t *)"ok\r", sizeof("ok\r"));
#endif

    free(temp);
}

// Bluetooth Classic Callback
#if BLUETOOTH_CLASSIC
void BLESerialCallBack(esp_spp_cb_event_t event, esp_spp_cb_param_t *param)
{

    if (event == ESP_SPP_SRV_OPEN_EVT)
    {
        debug("Client Connected has address: %02X:%02X:%02X:%02X:%02X:%02X",
              param->srv_open.rem_bda[0],
              param->srv_open.rem_bda[1],
              param->srv_open.rem_bda[2],
              param->srv_open.rem_bda[3],
              param->srv_open.rem_bda[4],
              param->srv_open.rem_bda[5]);
    }

    if (event == ESP_SPP_CLOSE_EVT)
    {
        debug("Client disconnected...");
    }

    if (event == ESP_SPP_WRITE_EVT)
    {
        debug("Sent data to Client...");
    }
}
#endif

// BLE callbacks
#if BLE
class MyServerCallbacks : public BLEServerCallbacks
{
    void onConnect(BLEServer *pServer)
    {
        // deviceConnected = true;
        debug("Client connected...");
    };

    void onDisconnect(BLEServer *pServer)
    {
        // deviceConnected = false;
        debug("Client disconnected...");
    }
};

class MyCallbacks : public BLECharacteristicCallbacks
{
    void onWrite(BLECharacteristic *pCharacteristic)
    {
        std::string value = pCharacteristic->getValue();
        String value_str;

        if (value.length() > 0)
        {
            for (int i = 0; i < value.length(); i++)
            {
                value_str = value_str + value[i];
            }

            debug("*********");
            debug("value = %s", value_str.c_str());

            // TODO: TBD
            parse_lte_msg((const char *)value_str.c_str());
        }
    }
};
#endif

// define tasks for Blink & AnalogRead
void TaskBLE(void *pvParameters);
void taskLed(void *pvParameters);
void taskTimer(void *pvParameters);
void taskCheckMode(void *pvParameters);

// the setup function runs once when you press reset or power the board
void setup()
{

    // initialize serial communication at 115200 bits per second:
    Serial.begin(115200);

    // configure LED PWM functionalitites
    ledcSetup(CHANNEL_RED, PWM_PREQ, RESOLUTION);
    ledcSetup(CHANNEL_GREEN, PWM_PREQ, RESOLUTION);
    ledcSetup(CHANNEL_BLUE, PWM_PREQ, RESOLUTION);

    // attach the channel to the GPIO to be controlled
    ledcAttachPin(PIN_RED, CHANNEL_RED);
    ledcAttachPin(PIN_GREEN, CHANNEL_GREEN);
    ledcAttachPin(PIN_BLUE, CHANNEL_BLUE);

#if DEBUG_MODE
    mutex_v = xSemaphoreCreateMutex();
#endif
    mutex_led_mode = xSemaphoreCreateMutex();
    mutex_color = xSemaphoreCreateMutex();

#if BLUETOOTH_CLASSIC
    // BLE setting
    SerialBT.register_callback(BLESerialCallBack);
    if (!SerialBT.begin("ESP32"))
    {
        debug("An error occurred initializing Bluetooth");
    }
    else
    {
        debug("Bluetooth initialized");
    }
    debug("The device started, now you can pair it with bluetooth!");
#endif

#if BLE
    // initialize BLE mode
    // Create the BLE Device
    BLEDevice::init("MyESP32");

    // Create the BLE Server
    pServer = BLEDevice::createServer();
    pServer->setCallbacks(new MyServerCallbacks());

    // Create the BLE Service
    BLEService *pService = pServer->createService(SERVICE_UUID);

    // Create a BLE Characteristic
    pCharacteristic = pService->createCharacteristic(
        CHARACTERISTIC_UUID,
        BLECharacteristic::PROPERTY_READ |
            BLECharacteristic::PROPERTY_WRITE |
            BLECharacteristic::PROPERTY_NOTIFY |
            BLECharacteristic::PROPERTY_INDICATE);

    // Create a BLE Descriptor
    pCharacteristic->addDescriptor(new BLE2902());
    // Esta línea es para el WriteStrings:
    pCharacteristic->setCallbacks(new MyCallbacks());

    // Start the service
    pService->start();

    // Start advertising
    BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
    pAdvertising->addServiceUUID(SERVICE_UUID);
    pAdvertising->setScanResponse(false);
    pAdvertising->setMinPreferred(0x0); // set value to 0x00 to not advertise this parameter
    BLEDevice::startAdvertising();
    debug("Waiting a client connection to notify...");
#endif

    xTaskCreatePinnedToCore(
        TaskBLE,   //
        "TaskBLE", // A name just for humans
        10000,     // This stack size can be checked & adjusted by reading the Stack Highwater
        NULL,      //
        2,         // Priority, with 3 (configMAX_PRIORITIES - 1) being the highest, and 0 being the lowest.
        NULL,
        ARDUINO_RUNNING_CORE);

    xTaskCreatePinnedToCore(
        taskLed,   //
        "taskLed", //
        10000,     // Stack size
        NULL,      //
        2,         // Priority
        NULL,
        ARDUINO_RUNNING_CORE);

    xTaskCreatePinnedToCore(
        taskTimer,   //
        "taskTimer", //
        10000,       // Stack size
        NULL,        //
        2,           // Priority
        NULL,
        ARDUINO_RUNNING_CORE);

#if DEBUG_MODE
    // xTaskCreatePinnedToCore(
    //     taskCheckMode,            //
    //     "taskCheckMode",          //
    //     5000, // Stack size
    //     NULL,                     //
    //     1,                        // Priority
    //     NULL,
    //     ARDUINO_RUNNING_CORE);
#endif
}

void loop()
{
    // Empty. Things are done in Tasks.
}

/*---------------------- Tasks ---------------------*/

void TaskBLE(void *pvParameters)
{
    (void)pvParameters;
    // char data[MAX_BLE_MSG_LENGTH] = {0};
    char *data = (char *)malloc(MAX_BLE_MSG_LENGTH);
    char inChar;
    uint8_t i = 0;

    for (;;)
    {
#if BLUETOOTH_CLASSIC
        if (SerialBT.available())
        {
            inChar = (char)SerialBT.read();
            // data[i] = inChar;
            // debug("%c", inChar);
            // i += 1;

            if ('\n' != inChar)
            {
                data[i] = inChar;
                i += 1;
            }

            if (END_OF_DATA_CHAR == inChar)
            {
                // data[i + 1] = '\0';
                data[i] = '\0';
                debug("BLE data received: %s", data);
                parse_lte_msg((const char *)data);
                i = 0;
                vTaskDelay(100 / portTICK_PERIOD_MS);
                debug("TaskBLE uxTaskGetStackHighWaterMark %d", uxTaskGetStackHighWaterMark(NULL));
            }
        }
        else
            vTaskDelay(100 / portTICK_PERIOD_MS);

        // max length
        if (i == (MAX_BLE_MSG_LENGTH - 2))
        {
            data[0] = '\0';
            i = 0;
        }
#endif
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }

    free(data);
}

void taskLed(void *pvParameters)
{
    (void)pvParameters;

    for (;;)
    {
        // switch ??
        if (FLASH == get_led_mode())
        {
            flash_led();
        }
        else if (FADE == get_led_mode())
        {
            fading_led();
        }
        else if (DIMMER == get_led_mode())
        {
            dimming_led();
        }
        // else if (TIMER == get_led_mode())
        // {
        //     timer_led();
        // }
        else if (COLOR_COLLECTION == get_led_mode())
        {
            color_collection();
        }
        else if (OFF == get_led_mode())
        {
            color_off();
            debug("taskLed uxTaskGetStackHighWaterMark %d", uxTaskGetStackHighWaterMark(NULL));
        }
        else
        {
            vTaskDelay(100 / portTICK_PERIOD_MS);
        }
    }
}

void taskTimer(void *pvParameters)
{

    (void)pvParameters;
    for (;;)
    {
        if (isTimer)
        {
            isTimerReset = false;
            debug("Timer : %d (s)", (uint8_t)timer);
            for (uint8_t i = 0; i < timer * 10; i++)
            {
                if (!isTimerReset)
                {
                    vTaskDelay(100 / portTICK_PERIOD_MS);
                    if ((0 != i) && (0 == (i % 10)))
                    {
                        debug("Timer expried in: %d (s)", (uint8_t)(timer - i / 10));
                        // debug("Timer expried in: %d (s)", (uint8_t)(timer));
                        debug("taskTimer uxTaskGetStackHighWaterMark %d", uxTaskGetStackHighWaterMark(NULL));
                    }
                }
                else
                {
                    debug("-----restart timer-----");
                    break;
                }
            }
            if (!isTimerReset)
            {
                // color_off();
                debug("-------Timer expried, set led OFF------------");
                set_led_mode(OFF);
                isTimer = false;
            }
        }
        else
        {
            vTaskDelay(100 / portTICK_PERIOD_MS);
        }
    }
}
/* this is the debug task*/
void taskCheckMode(void *pvParameters)
{
    (void)pvParameters;
    led_mode_t old_mode = NONE;
    // bool oldConnectionState = false;
    for (;;)
    {
        if (old_mode != get_led_mode())
        {
            old_mode = get_led_mode();
            debug("DEBUG LED MODE: %d", (uint8_t)get_led_mode());
        }
        // if (oldConnectionState != isConnected)
        // {
        //     oldConnectionState = isConnected;
        //     debug("BLE status: %s", (isConnected == true) ? "Connected" : "Disconnected");
        // }
        // vTaskDelay(500 / portTICK_PERIOD_MS);
    }
}