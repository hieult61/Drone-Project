#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_system.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "driver/ledc.h"

#include "esp_camera.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "camera_pins.h"
#include "connect_wifi.h"

static const char *TAG = "esp32-cam Webserver";

#define INTR_PIN 16
#define INTR_BIT_MASK (1ULL << INTR_PIN)
#define DEFAULT_INTR_FLAG 0
#define R_LED 12
#define G_LED 14

#define PART_BOUNDARY "123456789000000000000987654321"
static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* _STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* _STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

#define CONFIG_XCLK_FREQ 10000000
TaskHandle_t LED_TaskHandler;
SemaphoreHandle_t semaphore;
QueueHandle_t queue;

static void IRAM_ATTR gpio_interrupt_handler(void* arg)
{
    int pin_num = (int)arg;
    xSemaphoreGiveFromISR(semaphore, NULL);
    xQueueSendFromISR(queue,&pin_num,NULL);
    //xSemaphoreGiveFromISR(semaphore2,NULL);
}

void config_interrupt()
{
    semaphore = xSemaphoreCreateBinary();
    //semaphore2 = xSemaphoreCreateBinary();
    queue = xQueueCreate(5,sizeof(int));
    gpio_config_t intr_config = {
        .pin_bit_mask = INTR_BIT_MASK,
        .mode = GPIO_MODE_DEF_INPUT,
        .pull_up_en = 0,
        .pull_down_en = 1,
        .intr_type = GPIO_INTR_POSEDGE
    };
    gpio_config (&intr_config);
    gpio_install_isr_service(DEFAULT_INTR_FLAG);
    gpio_isr_handler_add(INTR_PIN, gpio_interrupt_handler, (void*)INTR_PIN);
    ESP_LOGI (TAG,"Interrupt config completed!\n");
}

void config_LED_SMD()
{
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK
    };
    ledc_timer_config(&timer);

    ledc_channel_config_t channel_0 = {
        .gpio_num = R_LED,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0
    };
    ledc_channel_config(&channel_0);

    ledc_channel_config_t channel_1 = {
        .gpio_num = G_LED,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_1,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0
    };
    ledc_channel_config(&channel_1);
    ESP_LOGI(TAG,"LED config completed!\n");
}

void LED_blink()
{
    ledc_set_fade_time_and_start(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 1024,1000, LEDC_FADE_WAIT_DONE);
    ledc_set_fade_time_and_start(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0,1000, LEDC_FADE_WAIT_DONE);
    
    vTaskDelay (10/portTICK_PERIOD_MS);

    ledc_set_fade_time_and_start(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, 1024,1000, LEDC_FADE_WAIT_DONE);
    ledc_set_fade_time_and_start(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, 0,1000, LEDC_FADE_WAIT_DONE);
}

void LED_Task (void* arg)
{
    int gpio_num;
    while(1)
    {
        //if(xSemaphoreTake(semaphore2, portMAX_DELAY))
        if (xQueueReceive(queue,&gpio_num,portMAX_DELAY))
        {
            ESP_LOGI(TAG,"Interrupt!\n");
            gpio_isr_handler_remove(INTR_PIN);
            do{
                vTaskDelay(20/portTICK_PERIOD_MS);
            }while (gpio_get_level(INTR_PIN) == 1);
            LED_blink();
            gpio_isr_handler_add(INTR_PIN, gpio_interrupt_handler, (void*)INTR_PIN);
        }
    }
}

void LED_SMD()
{
    xTaskCreate(LED_Task,"LED_Task",512,NULL,1,&LED_TaskHandler);
    ESP_LOGI(TAG,"LED Task Created!\n");
}

static esp_err_t init_camera(void)
{
    camera_config_t camera_config = {
        .pin_pwdn  = CAM_PIN_PWDN,
        .pin_reset = CAM_PIN_RESET,
        .pin_xclk = CAM_PIN_XCLK,
        .pin_sccb_sda = CAM_PIN_SIOD,
        .pin_sccb_scl = CAM_PIN_SIOC,

        .pin_d7 = CAM_PIN_D7,
        .pin_d6 = CAM_PIN_D6,
        .pin_d5 = CAM_PIN_D5,
        .pin_d4 = CAM_PIN_D4,
        .pin_d3 = CAM_PIN_D3,
        .pin_d2 = CAM_PIN_D2,
        .pin_d1 = CAM_PIN_D1,
        .pin_d0 = CAM_PIN_D0,
        .pin_vsync = CAM_PIN_VSYNC,
        .pin_href = CAM_PIN_HREF,
        .pin_pclk = CAM_PIN_PCLK,

        .xclk_freq_hz = CONFIG_XCLK_FREQ,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,

        .pixel_format = PIXFORMAT_JPEG,
        .frame_size = FRAMESIZE_VGA,

        .jpeg_quality = 10,
        .fb_count = 2,
        .grab_mode = CAMERA_GRAB_LATEST};//.CAMERA_GRAB_WHEN_EMPTY Sets when buffers should be filled
    esp_err_t err = esp_camera_init(&camera_config);
    if (err != ESP_OK)
    {
        return err;
    }
    return ESP_OK;
}

esp_err_t jpg_stream_httpd_handler(httpd_req_t *req){
    camera_fb_t * fb = NULL;
    esp_err_t res = ESP_OK;
    size_t _jpg_buf_len;
    uint8_t * _jpg_buf;
    char * part_buf[64];
    //static int64_t last_frame = 0;
    //if(!last_frame) {
    //    last_frame = esp_timer_get_time();}

    res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
    if(res != ESP_OK){
        return res;
    }

    while(true){
        if (xSemaphoreTake(semaphore,portMAX_DELAY)){
        fb = esp_camera_fb_get();
        if (!fb) {
            ESP_LOGE(TAG, "Camera capture failed");
            res = ESP_FAIL;
            break;
        }
        if(fb->format != PIXFORMAT_JPEG){
            bool jpeg_converted = frame2jpg(fb, 80, &_jpg_buf, &_jpg_buf_len);
            if(!jpeg_converted){
                ESP_LOGE(TAG, "JPEG compression failed");
                esp_camera_fb_return(fb);
                res = ESP_FAIL;
            }
        } else {
            _jpg_buf_len = fb->len;
            _jpg_buf = fb->buf;
        }

        if(res == ESP_OK){
            res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
        }
        if(res == ESP_OK){
            size_t hlen = snprintf((char *)part_buf, 64, _STREAM_PART, _jpg_buf_len);

            res = httpd_resp_send_chunk(req, (const char *)part_buf, hlen);
        }
        if(res == ESP_OK){
            res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);
        }
        if(fb->format != PIXFORMAT_JPEG){
            free(_jpg_buf);
        }
        esp_camera_fb_return(fb);
        if(res != ESP_OK){
            break;
        }
        //int64_t fr_end = esp_timer_get_time();
        //int64_t frame_time = fr_end - last_frame;
        //last_frame = fr_end;
        //frame_time /= 1000;
        //ESP_LOGI(TAG, "MJPG: %uKB %ums (%.1ffps)",(uint32_t)(_jpg_buf_len/1024),(uint32_t)(frame_time), 1000.0 / (uint32_t)(frame_time));
    }
    }
    //last_frame = 0;
    return res;
    //return ESP_OK;
}

esp_err_t take_photo (httpd_req_t *req)
{
    /*camera_fb_t *fb = NULL;

    while (true)
    {
        if (xSemaphoreTake(semaphore,portMAX_DELAY)){
            // Capture photo
            fb = esp_camera_fb_get();
            if (!fb) {
                ESP_LOGE(TAG, "Camera capture failed");
                httpd_resp_send_500(req);
                return ESP_FAIL;
            }

            // Send photo as response
            httpd_resp_set_type(req, "image/jpeg");
            httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
            httpd_resp_send(req, (const char *)fb->buf, fb->len);

            // Return the frame buffer
            esp_camera_fb_return(fb);
        }
    }*/
    return ESP_OK;
}

esp_err_t send_web_page(httpd_req_t *req)
{
    /*int response;
    int red = 0, green = 0, blue = 0;
    char response_data[sizeof(html_page) + 50];
    memset(response_data, 0, sizeof(response_data));
    sprintf(response_data, html_page);
    response = httpd_resp_send(req, response_data, HTTPD_RESP_USE_STRLEN);*/
    //return response;
    return ESP_OK;
}

esp_err_t show_result (httpd_req_t *req)
{
    return send_web_page(req);
}

httpd_uri_t uri_get = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = jpg_stream_httpd_handler,
    .user_ctx = NULL};

httpd_uri_t uri_photo = {
    .uri = "/photo",
    .method = HTTP_GET,
    .handler = take_photo,
    .user_ctx = NULL};

httpd_uri_t uri_result = {
    .uri = "/result",
    .method = HTTP_GET,
    .handler = show_result,
    .user_ctx = NULL};

httpd_handle_t setup_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server  = NULL;

    if (httpd_start(&server , &config) == ESP_OK)
    {
        httpd_register_uri_handler(server , &uri_get);
        httpd_register_uri_handler(server , &uri_photo);
        httpd_register_uri_handler(server , &uri_result);
    }

    return server;
}

void app_main()
{
    esp_err_t err;

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    connect_wifi();

    if (wifi_connect_status)
    {
        err = init_camera();
        if (err != ESP_OK)
        {
            printf("err: %s\n", esp_err_to_name(err));
            return;
        }
        setup_server();
        ESP_LOGI(TAG, "ESP32 CAM Web Server is up and running\n");
    }
    else
        ESP_LOGI(TAG, "Failed to connected with Wi-Fi, check your network Credentials\n");
    
    //Initialize other functions
    ledc_fade_func_install(0);
    config_interrupt();
    config_LED_SMD();
    LED_SMD();
}
