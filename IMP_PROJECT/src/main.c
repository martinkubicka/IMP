/*
 * @file main.c
 * @author Martin Kubička (xkubic45)
 * @date 15.12.2023
 * @brief Implementation of ESP32 tool for measuring temperature and displaying it on web page.
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/adc.h"
#include <esp_wifi.h>
#include <esp_http_server.h>
#include <esp_system.h>
#include <esp_event.h>
#include <nvs_flash.h>
#include "rtc.h"
#include "driver/rtc_io.h"
#include "esp_sleep.h"
#include <math.h>
#include <sys/time.h>
#include <lwip/apps/sntp.h>
#include "esp_http_client.h"
#include <string.h>
#include "esp_wifi.h"
#include "esp_log.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "cJSON.h"

/************************ MACROS AND GLOBAL VARIABLES ************************/

// adc 
#define GPIO_39 ADC1_CHANNEL_3 // gpio39 on adc
#define GPIO_LED 4 // gpio port where led is connected

// wifi access point
#define WIFI_SSID "ESP32" // wifi access point name

// temperature
double temperature = 0.0; // actual temperature 
char temperature_arr[10][48] = {'\0'}; // last 10 temperatures

// threshold
#define THRESHOLD_DEFAULT_VALUE -50.0 // default value of threshold
double threshold = THRESHOLD_DEFAULT_VALUE; // threshold value
bool threshold_overcome = false; // variable which indicates if threshold can be overcommed

// time
time_t t = 0; // actual time
char time_as_string[64]; // actual time as string

// memory
char* memory_name = "temp_mem"; // name of memory
char* memory_temperature = "t"; // temperature memory key
char* memory_count = "count"; // count memory key

// error handling
esp_err_t err;

/************************ ADC ************************/

/**
 * @brief Function for configuartion of ADC.
 */
void configure_adc() {
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(GPIO_39, ADC_ATTEN_DB_11);
}

/************************ LED ************************/

/**
 * @brief Function for configuration of LED.
 */
void configure_led() {
    gpio_config_t config = {
        .pin_bit_mask = (1ULL << GPIO_LED),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&config);
    gpio_set_level(GPIO_LED, 0);
}

/************************ TEMPERATURE AND THRESHOLD ************************/

/**
 * @brief Function for getting temperature.
 */
void get_temperature() {
    int mV = adc1_get_raw(GPIO_39);

    temperature = (8.194 - sqrt(pow(-8.194, 2) + 4 * 0.00262 * (1324 - mV))) / (2 * -0.00262) + 40;

    printf("Temperature: %.2f °C\n", temperature);
}

/**
 * @brief Function for handling threshold.
 */
void handle_threshold() {
    if (threshold != THRESHOLD_DEFAULT_VALUE) {
        // temperature is higher than threshold or 
        // if threshold was set still make led on if temperature is higher than threshold - 1.0
        if (temperature >= threshold || (threshold_overcome && temperature >= threshold - 1.0)) {
            threshold_overcome = true;
            gpio_set_level(GPIO_LED, 1); 
        } else {
            threshold_overcome = false;
            gpio_set_level(GPIO_LED, 0);
        }
    }
}

/************************ WIFI STATION ************************/

/**
 * @brief Function for configuration of wifi station. 
 *        Inspired by: https://medium.com/@fatehsali517/how-to-connect-esp32-to-wifi-using-esp-idf-iot-development-framework-d798dc89f0d6
 */
void wifi_configuration_station()
{
    err = esp_netif_init();
    if (err != ESP_OK) {
        printf("Error - esp_netif_init(): %s\n", esp_err_to_name(err));
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK) {
        printf("Error - esp_event_loop_create_default(): %s\n", esp_err_to_name(err));
    }

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wifi_initiation = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&wifi_initiation);  
    if (err != ESP_OK) {
        printf("Error - esp_wifi_init(): %s\n", esp_err_to_name(err));
    }

    wifi_config_t wifi_configuration = {
        .sta = {
            .ssid = "TP-LINK_8DBC",
            .password = "38599279",
           }
        };
    
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        printf("Error - esp_wifi_set_mode(): %s\n", esp_err_to_name(err));
    }

    err = esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_configuration);
    if (err != ESP_OK) {
        printf("Error - esp_wifi_set_config(): %s\n", esp_err_to_name(err));
    }

    err = esp_wifi_start();
    if (err != ESP_OK) {
        printf("Error - esp_wifi_start(): %s\n", esp_err_to_name(err));
    }

    err = esp_wifi_connect();
    if (err != ESP_OK) {
        printf("Error - esp_wifi_connect(): %s\n", esp_err_to_name(err));
    }
}

/************************ RTC ************************/

/**
 * @brief Function for initliazation of server time using NTP protocol.
 *        Inspired by: https://stackoverflow.com/questions/56025619/how-to-resync-time-from-ntp-server-in-esp-idf
 */
void init_time() {
    time_t now = 0;
    struct tm timeinfo = {0};
    int retry = 0;
    int retry_count = 10;

    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "sk.pool.ntp.org");
    sntp_init();

    while(timeinfo.tm_year < (2016 - 1900) && ++retry < retry_count)
    {
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        time(&now);
        localtime_r(&now, &timeinfo);
    }

    printf("Time initialized to: %s", asctime(&timeinfo));

    timeinfo.tm_hour += 1; // ours time zone
    t = mktime(&timeinfo);
}

/**
 * @brief Function for getting actual time based on RTC.
 */
void get_time() {
    time_t rtc_sec = esp_rtc_get_time_us() / 1000000;
    
    time_t actual_time = t + rtc_sec;
    struct tm *time = localtime(&actual_time);

    snprintf(time_as_string, sizeof(time_as_string),
             "%04d-%02d-%02d %02d:%02d:%02d ",
             time->tm_year + 1900, time->tm_mon + 1, time->tm_mday,
             time->tm_hour, time->tm_min, time->tm_sec);
}

/************************ NON-VOLATILE MEMORY ************************/

/**
 * @brief Function for clearing memory.
 */
void clear_memory() {
    nvs_handle_t memory_handle;
    err = nvs_open(memory_name, NVS_READWRITE, &memory_handle);
    if (err != ESP_OK) {
        printf("Error - nvs_open(): %s\n", esp_err_to_name(err));
    }

    err = nvs_erase_all(memory_handle);
    if (err != ESP_OK) {
        printf("Error - nvs_erase_all(): %s\n", esp_err_to_name(err));
    }

    nvs_close(memory_handle);
}

/**
 * @brief Function for storing actual temperature to memory.
 *        Inspired by: https://github.com/espressif/esp-idf/blob/master/examples/storage/nvs_rw_value/main/nvs_value_example_main.c
 */
void store_temperature() {
    nvs_handle_t memory_handle;
    uint16_t count = 0;

    err = nvs_open(memory_name, NVS_READWRITE, &memory_handle);
    if (err != ESP_OK) {
        printf("Error - nvs_open(): %s\n", esp_err_to_name(err));
    }

    err = nvs_get_u16(memory_handle, memory_count, &count);
    if (err != ESP_OK) { // for example memory is full -> clear memory
        printf("Error - nvs_get_u16: %s -> clearing memory\n", esp_err_to_name(err));
        clear_memory();
        count = 0;
    } else {
        ++count;
    }

    // time
    get_time();
    char temperature_as_string[8];
    snprintf(temperature_as_string, sizeof(temperature_as_string), "%.2f", temperature);

    // time + temperature
    char record[strlen(time_as_string) + strlen(temperature_as_string) + 1];
    strcpy(record, time_as_string);
    strcat(record, temperature_as_string);

    // temperature key for memory
    char temperature_key[strlen(memory_temperature) + 2];
    strcpy(temperature_key, memory_temperature);
    snprintf(temperature_key + strlen(memory_temperature), 8, "%d", count);

    err = nvs_set_str(memory_handle, temperature_key, record);
    if (err != ESP_OK) { // for example memory is full -> clear memory
        printf("Error - nvs_set_str: %s -> clearing memory\n", esp_err_to_name(err));
        clear_memory();
        count = 0;
    }

    // storing actual count of records in memory so I can get last 10 records
    err = nvs_set_u16(memory_handle, memory_count, count);
    if (err != ESP_OK) { // for example memory is full -> clear memory
        printf("Error - nvs_set_u16: %s -> clearing memory\n", esp_err_to_name(err));
        clear_memory();
        count = 0;
    }

    err = nvs_commit(memory_handle);
    if (err != ESP_OK) {
        printf("Error - nvs_commit(): %s\n", esp_err_to_name(err));
    }

    nvs_close(memory_handle);
}

/**
 * @brief Get the last 10 temperatures from memory.
 *        Inspired by: https://github.com/espressif/esp-idf/blob/master/examples/storage/nvs_rw_value/main/nvs_value_example_main.c
 */
void get_last_10_temperatures_from_nvm() {
    nvs_handle_t memory_handle;
    uint16_t count = 0; // records in memory
    size_t s;
    char temperature_key[strlen(memory_temperature) + 2]; // temperature key for actual record

    err = nvs_open(memory_name, NVS_READWRITE, &memory_handle);
    if (err != ESP_OK) {
        printf("Error - nvs_open(): %s\n", esp_err_to_name(err));
    }

    err = nvs_get_u16(memory_handle, memory_count, &count); // get count of records in memory
    if (err != ESP_OK) {
        printf("Error - nvs_get_u16: %s\n", esp_err_to_name(err));
    }

    int index = 0; // index for temperature_arr
    // getting last 10 records
    for (size_t i = count; i > (count - 10 >= 0 ? count - 10 : -1); --i) {
        strcpy(temperature_key, memory_temperature);
        snprintf(temperature_key + strlen(memory_temperature), 8, "%d", (unsigned int)count - index);
        err = nvs_get_str(memory_handle, temperature_key, temperature_arr[count - i], &s);

        if (err != ESP_OK) {
            printf("Error - nvs_get_str: %s\n", esp_err_to_name(err));
        }

        ++index;
    }

    nvs_close(memory_handle);
}

/************************ WIFI ACCESS POINT ************************/

/**
 * @brief Function for handling get root request.
 * 
 * @param req request
 * 
 * @return esp_err_t esp state
 */
esp_err_t get_root_handler(httpd_req_t *req) {
    char response[2048];

    // html string of page
    snprintf(response, sizeof(response),
             "<!DOCTYPE HTML><html><head><title>ESP32 - Temperature tool</title><meta name='viewport' content='width=device-width, initial-scale=1.0' charset='UTF-8'><style> h2, p { font-family: Arial, sans-serif; }</style><script>function updateTemperature(){fetch('/get_temperature').then(response => response.text()).then(newTemperature => {document.getElementById('temperature').innerText = newTemperature + ' \u00B0C';});}function setThreshold(){var thresholdValue = document.getElementById('threshold').value;fetch('/set_threshold',{method: 'POST',headers: {'Content-Type': 'application/x-www-form-urlencoded',},body: 'threshold=' + thresholdValue,}); var inputElement = document.getElementById('threshold_button'); var paragraph = document.createElement('p'); paragraph.textContent = 'Threshold set successfully.'; inputElement.insertAdjacentElement('afterend', paragraph); setTimeout(function() { paragraph.parentNode.removeChild(paragraph);}, 3000); }function getLast10Temperatures(){fetch('/get_last_10_temperatures').then(response => response.json()).then(data => { const temperatureContainer = document.getElementById('temperature-container'); temperatureContainer.innerHTML = ''; for (let i = 0; i < data.temperature.length; i++) { const newParagraph = document.createElement('p'); newParagraph.innerText = data.temperature[i]; temperatureContainer.appendChild(newParagraph);}});} setInterval(updateTemperature, 2000);setInterval(getLast10Temperatures, 2000);getLast10Temperatures();updateTemperature();</script></head><body><h2>Actual temperature:</h2><p id='temperature'>%.2f &deg;C</p><h2>Set temperature threshold:</h2><input type='number' step='0.01' id='threshold' min='-50.00'> <button onclick='setThreshold()' id='threshold_button'>Set</button><br /><h2>Last 10 temperatures</h2><div id='temperature-container'></div></body></html>", temperature);

    err = httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
    if (err != ESP_OK) {
        printf("Error - get_root_handler - httpd_resp_send(): %s\n", esp_err_to_name(err));
    }
    
    return ESP_OK;
}

/**
 * @brief Function for handling get temperature request.
 * 
 * @param req request
 * 
 * @return esp_err_t esp state
 */
static esp_err_t get_temperature_handler(httpd_req_t *req) {
    char response[16];
    snprintf(response, sizeof(response), "%.2f", temperature);
    err = httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
    if (err != ESP_OK) {
        printf("Error - get_temperature_handler - httpd_resp_send(): %s\n", esp_err_to_name(err));
    }

    return ESP_OK;
}

/**
 * @brief Function for handling get last 10 temperatures request.
 * 
 * @param req request
 * @return esp_err_t esp state
 */
static esp_err_t get_last_10_temperatures_from_nvm_handler(httpd_req_t *req) {
    get_last_10_temperatures_from_nvm();

    cJSON *json_root = cJSON_CreateObject();
    cJSON *json_array = cJSON_CreateArray();

    for (int i = 0; i < 10; i++) {
        cJSON_AddItemToArray(json_array, cJSON_CreateString(temperature_arr[i]));
    }
    cJSON_AddItemToObject(json_root, "temperature", json_array);
    const char *json_str = cJSON_PrintUnformatted(json_root);

    err = httpd_resp_set_type(req, "application/json");
    if (err != ESP_OK) {
        printf("Error - get_last_10_temperatures_from_nvm_handler - httpd_resp_set_type(): %s\n", esp_err_to_name(err));
    }

    err = httpd_resp_send(req, json_str, strlen(json_str));
    if (err != ESP_OK) {
        printf("Error - get_last_10_temperatures_from_nvm_handler - httpd_resp_send(): %s\n", esp_err_to_name(err));
    }

    free((void *)json_str);
    cJSON_Delete(json_root);

    return ESP_OK;
}

/**
 * @brief Function for handling set threshold request.
 * 
 * @param req request
 * @return esp_err_t esp state
 */
static esp_err_t post_set_threshold_handler(httpd_req_t *req) {
    char buffer[20] = {'\0'};
    char thresh[10] = {'\0'};
    int len = req->content_len;
    int count = 0;

    httpd_req_recv(req, buffer, req->content_len);

    // getting just threshold value from request
    for (int i = 0; i < len; i++) {
        if (buffer[i] == '=') {
            count++;
        } else if (count != 0) {
            thresh[count - 1] = buffer[i];
            count++;
        }
    }

    gpio_set_level(GPIO_LED, 0);
    threshold = atof(thresh);
    printf("Threshold set to: %.2f\n", threshold);

    return ESP_OK;
}

/**
 * @brief Function for configuration of web server access point.
 *        Inspired by: https://esp32tutorials.com/esp32-web-server-esp-idf/
 */
void wifi_configuration_access_point() {
    err = esp_netif_init();
    if (err != ESP_OK) {
        printf("Error - esp_netif_init(): %s\n", esp_err_to_name(err));
    }

    esp_netif_create_default_wifi_ap();

    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&config);
    if (err != ESP_OK) {
        printf("Error - esp_wifi_init(): %s\n", esp_err_to_name(err));
    }

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = WIFI_SSID,
            .ssid_len = strlen(WIFI_SSID),
            .authmode = WIFI_AUTH_OPEN,
            .max_connection = 10,
        },
    };
    
    err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err != ESP_OK) {
        printf("Error - esp_wifi_set_mode(): %s\n", esp_err_to_name(err));
    }

    err = esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    if (err != ESP_OK) {
        printf("Error - esp_wifi_set_config(): %s\n", esp_err_to_name(err));
    }

    err = esp_wifi_start();
    if (err != ESP_OK) {
        printf("Error - esp_wifi_start(): %s\n", esp_err_to_name(err));
    }
}

/**
 * @brief Function where endpoints are defined.
 *        Inspired by: https://esp32tutorials.com/esp32-web-server-esp-idf/
 * 
 * @return httpd_handle_t server
 */
httpd_handle_t define_endpoints() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;

    if (httpd_start(&server, &config) == ESP_OK) {
        // root
        httpd_uri_t get_root = {
            .uri      = "/",
            .method   = HTTP_GET,
            .handler  = get_root_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &get_root);

        // get temperature
        httpd_uri_t get_temperature = {
            .uri      = "/get_temperature",
            .method   = HTTP_GET,
            .handler  = get_temperature_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &get_temperature);

        // set threshold
        httpd_uri_t set_threshold = {
            .uri       = "/set_threshold",
            .method    = HTTP_POST,
            .handler   = post_set_threshold_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &set_threshold);

        // get last 10 temperatures
        httpd_uri_t get_last_10_temperatures = {
            .uri      = "/get_last_10_temperatures",
            .method   = HTTP_GET,
            .handler  = get_last_10_temperatures_from_nvm_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &get_last_10_temperatures);
    }

    return server;
}

/**
 * @brief Function for stopping webserver access point.
 * 
 * @param server server
 */
void stop_webserver(httpd_handle_t server) {
    err = httpd_stop(server);
    if (err != ESP_OK) {
        printf("Error - httpd_stop(): %s\n", esp_err_to_name(err));
    }
}

/************************ MAIN ************************/

void app_main() {    
    err = nvs_flash_init();
    if (err != ESP_OK) {
        printf("Error - nvs_flash_init(): %s\n", esp_err_to_name(err));
    }

    // wifi station
    wifi_configuration_station();

    // rtc
    init_time();

    // wifi access point
    wifi_configuration_access_point();
    httpd_handle_t server = define_endpoints();
    
    // adc
    configure_adc();

    // led
    configure_led();
    
    printf("Program initialized successfully.\n");

    while (1) {
        get_temperature();
        handle_threshold();
        store_temperature();

        vTaskDelay(2000 / portTICK_PERIOD_MS);
    }

    stop_webserver(server);
}

/*** End of main.c ***/
