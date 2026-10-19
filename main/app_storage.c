#include "app_storage.h"

#include "nvs.h"
#include "nvs_flash.h"

#define APP_STORAGE_NAMESPACE "ieq"
#define APP_STORAGE_THRESHOLDS_KEY "alarm_th"

esp_err_t app_storage_init(void)
{
    /* NVS 分区异常时擦除并重新初始化，保证首次启动可用。 */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    return ret;
}

esp_err_t app_storage_load_thresholds(app_alarm_thresholds_t *thresholds)
{
    /* 阈值作为一个 blob 保存，结构体字段变更时要注意兼容。 */
    if (thresholds == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(APP_STORAGE_NAMESPACE, NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        return ret;
    }

    size_t required_size = sizeof(*thresholds);
    ret = nvs_get_blob(handle, APP_STORAGE_THRESHOLDS_KEY, thresholds, &required_size);
    nvs_close(handle);
    return ret;
}

esp_err_t app_storage_save_thresholds(const app_alarm_thresholds_t *thresholds)
{
    if (thresholds == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(APP_STORAGE_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_blob(handle, APP_STORAGE_THRESHOLDS_KEY, thresholds, sizeof(*thresholds));
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);
    return ret;
}
