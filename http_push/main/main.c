#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_system.h"


#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "protocol_examples_common.h"

#include "esp_partition.h"

#include "esp_http_server.h"
#include "sif.h"

#define HTTP_CHUNK_SIZE 4096

static const char *TAG = "http_sif_ota";

static void reboot()
{
	ESP_LOGI(TAG, "rebooting in 1 seconds...");
	vTaskDelay(1000 / portTICK_PERIOD_MS);
	esp_restart();
}

static esp_err_t ota_post_handler(httpd_req_t *req)
{
	char *recv_buf = calloc(HTTP_CHUNK_SIZE, sizeof(char));
	if (!recv_buf) {
		return ESP_FAIL;
	}

	int ret, content_length = req->content_len;
	ESP_LOGI(TAG, "content length: %d B", content_length);

	int remaining = content_length;
	int64_t start = esp_timer_get_time();

	sif_opts_t opts = INIT_NEXT_OTA_PARTITION_SIF_OPTS();

#if CONFIG_SRC_FILE == 1
	opts.src.type = SIF_SRC_FILE;
	opts.src.where = CONFIG_SRC_FILENAME;
#endif

#if CONFIG_DEST_FILE == 1
	opts.dest.type = SIF_SRC_FILE;
	opts.dest.where = CONFIG_DEST_FILENAME;
#endif

#if CONFIG_PATCH_FILE == 1
	opts.patch.type = SIF_SRC_FILE;
	opts.patch.where = CONFIG_PATCH_FILENAME;
#endif

	sif_patch_writer_t *writer = NULL;

	size_t count = 0;
	while (remaining > 0) {
		if ((ret = httpd_req_recv(req, recv_buf, MIN(remaining, HTTP_CHUNK_SIZE))) <= 0) {
			if (ret == HTTPD_SOCK_ERR_TIMEOUT) // retry receiving if timeout occurred
				continue;
			goto ERROR;
		}

		if (!writer) {
			int err = sif_patch_init(&writer, opts.patch.where, content_length);
			if (err != SIF_OK) {
				ESP_LOGE(TAG, "error: %s", sif_error_as_string(err));
				goto ERROR;
			}
		}

		if (sif_patch_write(writer, recv_buf, ret) != ESP_OK)
			goto ERROR;

		count += ret;
		remaining -= ret;
		ESP_LOGI(TAG, "download progress: %0.2f %%", ((float)(content_length - remaining) / content_length) * 100);
	}

	sif_patch_free(writer);
	writer = NULL;

	ESP_LOGI(TAG, "download complete. total download time: %0.3f s", (float)(esp_timer_get_time() - start) / 1000000L);
	ESP_LOGI(TAG, "patch size: %uKB", count/1024);

	int err = sif_check_and_apply(content_length, &opts, NULL);
	if (err) {
		ESP_LOGE(TAG, "error: %s", sif_error_as_string(err));
		goto ERROR;
	}

	httpd_resp_send(req, NULL, 0);
	free(recv_buf);
	sif_patch_free(writer);

#if CONFIG_DEST_PARTITION == 1
	reboot();
#endif
	return ESP_OK;

ERROR:
	httpd_resp_send_500(req);
	free(recv_buf);
	sif_patch_free(writer);
	return ESP_OK;
}

static const httpd_uri_t ota =
{
	.uri = "/ota",
	.method = HTTP_POST,
	.handler = ota_post_handler,
	.user_ctx = NULL
};

static httpd_handle_t start_webserver(void)
{
	httpd_handle_t server = NULL;
	httpd_config_t config = HTTPD_DEFAULT_CONFIG();
	config.lru_purge_enable = true;

	ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);
	if (httpd_start(&server, &config) == ESP_OK) {
		ESP_LOGI(TAG, "Registering URI handlers");
		httpd_register_uri_handler(server, &ota);
		return server;
	}

	ESP_LOGI(TAG, "Error starting server!");
	return NULL;
}

void app_main(void)
{
	ESP_LOGI(TAG, "Initialising WiFi Connection...");

	ESP_ERROR_CHECK(nvs_flash_init());
	ESP_ERROR_CHECK(esp_netif_init());
	ESP_ERROR_CHECK(esp_event_loop_create_default());

#if CONFIG_SRC_FILE == 1 || CONFIG_DEST_FILE == 1 || CONFIG_PATCH_FILE == 1
	esp_vfs_spiffs_conf_t conf = {
		.base_path = "/spiffs",
		.partition_label = NULL,
		.max_files = 5,
		.format_if_mount_failed = true
	};

	// Use settings defined above to initialize and mount spiffs filesystem.
	// Note: esp_vfs_spiffs_register is an all-in-one convenience function.
	esp_err_t ret = esp_vfs_spiffs_register(&conf);

	if (ret != ESP_OK)
	{
		if (ret == ESP_FAIL)
			ESP_LOGE(TAG, "failed to mount or format filesystem");
		else if (ret == ESP_ERR_NOT_FOUND)
			ESP_LOGE(TAG, "failed to find spiffs partition");
		else
			ESP_LOGE(TAG, "failed to initialise spiffs (%s)", esp_err_to_name(ret));

		return;
	}

    size_t total = 0, used = 0;
    ret = esp_spiffs_info(conf.partition_label, &total, &used);
    if (ret != ESP_OK)
        ESP_LOGE(TAG, "failed to get spiffs partition information (%s)", esp_err_to_name(ret));
	else
        ESP_LOGI(TAG, "partition size: total: %d, used: %d", total, used);
#endif

	// This helper function configures Wi-Fi or Ethernet, as selected in menuconfig.
	// Read "Establishing Wi-Fi or Ethernet Connection" section in
	// examples/protocols/README.md for more information about this function.
	ESP_ERROR_CHECK(example_connect());

	ESP_LOGI(TAG, "setting up http server...");
	start_webserver();
}

