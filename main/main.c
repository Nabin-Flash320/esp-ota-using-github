/* WiFi station Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_crt_bundle.h"
#include "esp_https_ota.h"
#include "esp_app_format.h"
#include "esp_ota_ops.h"

#include "lwip/err.h"
#include "lwip/sys.h"

#include "esp_err.h"

#include "esp_http_client.h"

#define USERNAME "USERNAME"
#define PAT "PERSONAL_ACCESS_TOKEN"
#define REPOSITORY_NAME "REPOSITORY_NAME"
#define ASSET_ID 183937167

#define EXAMPLE_ESP_WIFI_SSID "SSID"
#define EXAMPLE_ESP_WIFI_PASS "PSWD"
#define EXAMPLE_ESP_MAXIMUM_RETRY 5
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA_WPA2_PSK

/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group;

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

static const char *TAG = "wifi station";

char ota_url[128];

static int s_retry_num = 0;

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        if (s_retry_num < EXAMPLE_ESP_MAXIMUM_RETRY)
        {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        }
        else
        {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG, "connect to the AP fail");
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = EXAMPLE_ESP_WIFI_SSID,
            .password = EXAMPLE_ESP_WIFI_PASS,
            /* Setting a password implies station will connect to all security modes including WEP/WPA.
             * However these modes are deprecated and not advisable to be used. Incase your Access point
             * doesn't support WPA2, these mode can be enabled by commenting below line */
            .threshold.authmode = ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
    if (bits & WIFI_CONNECTED_BIT)
    {
        ESP_LOGI(TAG, "connected to ap SSID:%s password:%s",
                 EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS);
    }
    else if (bits & WIFI_FAIL_BIT)
    {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s",
                 EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS);
    }
    else
    {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }

    /* The event will not be processed after unregister */
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip));
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id));
    vEventGroupDelete(s_wifi_event_group);
}

/*
curl -L \
  -H "Accept: application/vnd.github+json" \
  -H "Authorization: Bearer <Personal Access Token>" \
  -H "X-GitHub-Api-Version: 2022-11-28" \
  https://api.github.com/repos/<Username>/<Repository Name>/releases/latest
*/

static esp_err_t ota_http_client_init_callback(esp_http_client_handle_t http_client)
{
    if (NULL == http_client)
    {
        return ESP_FAIL;
    }

    char token[128];
    snprintf(token, 128, "Bearer %s", PAT);

    ESP_ERROR_CHECK(esp_http_client_set_header(http_client, "Accept", "application/octet-stream"));
    ESP_ERROR_CHECK(esp_http_client_set_header(http_client, "Authorization", token));
    ESP_ERROR_CHECK(esp_http_client_set_header(http_client, "X-GitHub-Api-Version", "2022-11-28"));

    return ESP_OK;
}

void ota_performer()
{
    memset(ota_url, 0, 128);

    // Prepare URL for the asset formatted as: https://api.github.com/repos/<Username>/<Repository Name>/releases/assets/<Asset ID>
    snprintf(ota_url, 128, "https://api.github.com/repos/%s/%s/releases/assets/%d", USERNAME, REPOSITORY_NAME, ASSET_ID);
    static esp_http_client_config_t ota_http_configurations = {
        .url = ota_url,
        .crt_bundle_attach = esp_crt_bundle_attach, // Attach certificate bundles provided by the IDF itself, no other certificates are required.
        .max_redirection_count = 2,                 // Client is then redirected to the different link to get the binary, so this has to be kept at least 1. Also, it may not be required.
        .method = HTTP_METHOD_GET,
        .buffer_size = 4096 * 2,
        .buffer_size_tx = 4096 * 2, // Since download is done partially, it is mandatory to provide tx buffer size else, out of buffer error is thrown that aborts the OTA.
        .keep_alive_enable = true,
        .keep_alive_interval = 20,
    };

    ESP_LOGE(__FILE__, "Starting OTA updates");
    static esp_https_ota_config_t ota_config = {
        .http_config = &ota_http_configurations,
        // HTTPS OTA initializes the http client, if call exits then it is called. Here when downloaing assets from github, following 3 additional headers have to be 
        // added in the request header
        // 1. `Accept: application/octet-stream`
        // 2. "Authorization: Bearer <Personal Access Token>"
        // 3. "X-GitHub-Api-Version: 2022-11-28"
        // So, this has to be provided and also, esp_https_ota() function cannot be called in this case.
        .http_client_init_cb = ota_http_client_init_callback,
        .partial_http_download = true,
    };

    esp_https_ota_handle_t https_ota_handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_config, &https_ota_handle);
    if (https_ota_handle == NULL)
    {
        ESP_LOGE(__FILE__, "OTA error(error: %s)", esp_err_to_name(err));
        goto ota_end;
    }

    esp_app_desc_t app_desc;
    err = esp_https_ota_get_img_desc(https_ota_handle, &app_desc);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_https_ota_read_img_desc failed");
        goto ota_end;
    }

    while (1) // Wait until OTA is not completed.
    {
        err = esp_https_ota_perform(https_ota_handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS)
        {
            break;
        }
        // esp_https_ota_perform returns after every read operation which gives user the ability to
        // monitor the status of OTA upgrade by calling esp_https_ota_get_image_len_read, which gives length of image
        // data read so far.
        ESP_LOGD(TAG, "Image bytes read: %d", esp_https_ota_get_image_len_read(https_ota_handle));
    }

    if (ESP_OK == err)
    {
        if (esp_https_ota_is_complete_data_received(https_ota_handle) != true)
        {
            // the OTA image was not completely received and user can customise the response to this situation.
            ESP_LOGE(TAG, "Complete data was not received.");
            goto ota_end;
        }
        else
        {
            ESP_LOGE(__FILE__, "OTA data received complete...(len: %d)", esp_https_ota_get_image_len_read(https_ota_handle));
            // This function call cleans up all the allocated memory and validates the firmware downloaded.
            esp_err_t ota_finish_err = esp_https_ota_finish(https_ota_handle);
            if (ota_finish_err == ESP_OK)
            {
                int count = 5;
                while (count > 0)
                {
                    printf("Rebooting the device in %d seconds.\n", count--);
                    vTaskDelay(1000 / portTICK_PERIOD_MS);
                }
                // After OTA is completed, restart the device to boot device with the new firmware
                esp_restart();
            }
            else
            {
                if (ota_finish_err == ESP_ERR_OTA_VALIDATE_FAILED)
                {
                    ESP_LOGE(TAG, "Image validation failed, image is corrupted");
                }
                ESP_LOGE(TAG, "ESP_HTTPS_OTA upgrade failed 0x%x", ota_finish_err);
                goto ota_end;
            }
        }
    }
    else
    {
        ESP_LOGE(__FILE__, "Error flashing OTA(Code: %s)", esp_err_to_name(err));
    }
ota_end:
    esp_https_ota_abort(https_ota_handle);
    vTaskDelete(NULL);
}

void app_main(void)
{

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "ESP_WIFI_MODE_STA");
    wifi_init_sta();

    xTaskCreate(ota_performer, "ota-apps", 4 * 2048, NULL, 2, NULL);
}
