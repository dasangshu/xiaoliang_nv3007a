#include "fs_manager.h"
#include <dirent.h>

static const char *TAG = "fs_manager";
static fs_type_t current_fs_type = FS_TYPE_SPIFFS;
static sdmmc_card_t *sd_card = NULL;

static esp_err_t init_spiffs(fs_config_t *config)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = config->spiffs.base_path,
        .partition_label = config->spiffs.partition_label,
        .max_files = config->spiffs.max_files,
        .format_if_mount_failed = config->spiffs.format_if_mount_failed
    };
    
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find SPIFFS partition");
        } else {
            ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        }
        return ret;
    }
    
    return ESP_OK;
}

static esp_err_t init_sdcard(fs_config_t *config)
{
    esp_err_t ret = ESP_OK;
    
    // SD卡挂载配置
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = config->sd_card.format_if_mount_failed,
        .max_files = config->sd_card.max_files,
        .allocation_unit_size = 16 * 1024
    };

    // SD卡主机配置
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    
    // SD卡插槽配置
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1; // 1-line SD模式
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    #ifdef SOC_SDMMC_USE_GPIO_MATRIX
    slot_config.clk = config->sd_card.clk;
    slot_config.cmd = config->sd_card.cmd;
    slot_config.d0 = config->sd_card.d0;
    #endif

    ESP_LOGI(TAG, "Mounting SD card to %s", config->sd_card.mount_point);
    ret = esp_vfs_fat_sdmmc_mount(config->sd_card.mount_point, &host, &slot_config, 
                                 &mount_config, &sd_card);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount SD card filesystem");
        } else {
            ESP_LOGE(TAG, "Failed to initialize SD card (%s)", esp_err_to_name(ret));
        }
        return ret;
    }

    sdmmc_card_print_info(stdout, sd_card);
    return ESP_OK;
}

esp_err_t fs_manager_init(fs_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    current_fs_type = config->type;
    
    if (config->type == FS_TYPE_SPIFFS) {
        return init_spiffs(config);
    } else if (config->type == FS_TYPE_SD_CARD) {
        return init_sdcard(config);
    }
    
    return ESP_ERR_INVALID_ARG;
}

void fs_manager_list_files(const char* path)
{
    DIR *dir = opendir(path);
    if (dir == NULL) {
        ESP_LOGE(TAG, "Failed to open directory %s", path);
        return;
    }
    
    while (true) {
        struct dirent *pe = readdir(dir);
        if (!pe) break;
        ESP_LOGI(TAG, "d_name=%s d_ino=%d d_type=%x", pe->d_name, pe->d_ino, pe->d_type);
    }
    closedir(dir);
}

void fs_manager_deinit(void)
{
    if (current_fs_type == FS_TYPE_SPIFFS) {
        esp_vfs_spiffs_unregister(NULL);
    } else if (current_fs_type == FS_TYPE_SD_CARD && sd_card != NULL) {
        const char *mount_point = "/sdcard";  // 使用默认挂载点
        esp_vfs_fat_sdcard_unmount(mount_point, sd_card);
        sd_card = NULL;
    }
}

fs_type_t fs_manager_get_type(void)
{
    return current_fs_type;
}