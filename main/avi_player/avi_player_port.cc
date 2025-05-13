#include "avi_player_port.h"
#include "esp_heap_caps.h"
#include "esp_jpeg_decode.h"
#include "fs_manager.h"
#include "board.h"
#include "freertos/semphr.h"  // 添加这行以支持信号量
static const char *TAG = "avi_player_port";

static i2s_chan_handle_t i2s_tx_handle = NULL;
static uint8_t *img_rgb565 = NULL;

static bool is_playing = false;
static char file_path[128] = {0};
static bool enable_loop = true;  // 默认启用循环播放

static SemaphoreHandle_t avi_mutex = NULL;  // 添加互斥锁


void video_write(frame_data_t *data, void *arg)
{
    int Rgbsize = 0;
    esp_jpeg_decode_one_picture(data->data, data->data_bytes, &img_rgb565, &Rgbsize);
    // 通过回调函数更新LCD显示
    if (img_rgb565 != NULL&& Rgbsize > 0) {
        auto display = Board::GetInstance().GetDisplay();
        display->SetFaceImage(img_rgb565, get_rgb_width(), get_rgb_height());
        free(img_rgb565);
    }
}

void audio_write(frame_data_t *data, void *arg)
{
    size_t bytes_write = 0;
    // i2s_channel_write(i2s_tx_handle, data->data, data->data_bytes, &bytes_write, 100);
}

static void play_end_cb(void *arg)
{
    ESP_LOGI(TAG, "播放结束");
    
    // 保存当前状态和文件路径的本地副本
    bool was_playing = is_playing;
    char current_path[128] = {0};
    strncpy(current_path, file_path, sizeof(current_path) - 1);
    
    // 标记播放已结束
    is_playing = false;
    
    // 如果启用了循环播放，尝试重新开始播放
    if (enable_loop && was_playing) {
        ESP_LOGI(TAG, "准备循环播放: %s", current_path);
        
        // 确保有足够的延迟让资源完全释放
        vTaskDelay(pdMS_TO_TICKS(200));
        
        if (avi_mutex == NULL) {
            ESP_LOGE(TAG, "互斥锁未初始化，循环播放可能不安全");
        } else if (xSemaphoreTake(avi_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
            // 检查文件是否存在
            FILE* f = fopen(current_path, "r");
            if (f != NULL) {
                fclose(f);
                ESP_LOGI(TAG, "文件存在，开始循环播放");
                
                // 尝试播放文件
                esp_err_t ret = avi_player_play_from_file(current_path);
                if (ret == ESP_OK) {
                    is_playing = true;
                    ESP_LOGI(TAG, "循环播放开始成功");
                } else {
                    ESP_LOGE(TAG, "循环播放失败: %s (错误码: %d)", esp_err_to_name(ret), ret);
                    
                    // 再次尝试，额外延迟
                    vTaskDelay(pdMS_TO_TICKS(500));
                    ret = avi_player_play_from_file(current_path);
                    if (ret == ESP_OK) {
                        is_playing = true;
                        ESP_LOGI(TAG, "第二次尝试循环播放成功");
                    } else {
                        ESP_LOGE(TAG, "第二次尝试循环播放失败: %s", esp_err_to_name(ret));
                    }
                }
            } else {
                ESP_LOGE(TAG, "循环播放失败: 文件不存在或无法访问: %s", current_path);
            }
            
            xSemaphoreGive(avi_mutex);
        } else {
            ESP_LOGE(TAG, "无法获取互斥锁，循环播放失败");
        }
    }
}
esp_err_t avi_player_port_init(avi_player_port_config_t *config)
{
    if (avi_mutex == NULL) {
        avi_mutex = xSemaphoreCreateMutex();
        if (avi_mutex == NULL) {
            ESP_LOGE(TAG, "创建互斥锁失败");
            return ESP_ERR_NO_MEM;
        }
    }

    // 使用SPIFFS
    fs_config_t spiffs_config = {
        .type = FS_TYPE_SPIFFS,
        .spiffs = {
            .base_path = "/spiffs",
            .partition_label = "storage",
            .max_files = 5,
            .format_if_mount_failed = true
        }
    };
    // 初始化文件系统
    ESP_ERROR_CHECK(fs_manager_init(&spiffs_config));  // 或 &sdcard_config

    // 列出文件
    fs_manager_list_files("/spiffs");  // 或 "/sdcard"

    img_rgb565 = (uint8_t *)heap_caps_malloc(240 * 280 * 2, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
    if (!img_rgb565)
    {
        return ESP_ERR_NO_MEM;
    }

    avi_player_config_t player_config = {
        .buffer_size = config->buffer_size,
        .video_cb = video_write,
        .audio_cb = audio_write,
        .avi_play_end_cb = play_end_cb,
        .coreID = config->core_id,
    };

    return avi_player_init(player_config);
}

esp_err_t avi_player_port_play_file(const char *filepath)
{
    ESP_LOGI(TAG, "尝试播放文件: %s", filepath);
    
    if (avi_mutex == NULL) {
        ESP_LOGE(TAG, "互斥锁未初始化");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (xSemaphoreTake(avi_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        ESP_LOGE(TAG, "无法获取互斥锁，可能存在死锁");
        return ESP_ERR_TIMEOUT;
    }
    
    // 检查文件是否存在
    FILE* f = fopen(filepath, "r");
    if (f == NULL) {
        ESP_LOGE(TAG, "文件不存在或无法访问: %s", filepath);
        xSemaphoreGive(avi_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    fclose(f);
    
    // 如果当前正在播放，先停止
    if (is_playing) {
        ESP_LOGI(TAG, "停止当前播放");
        avi_player_play_stop();
        is_playing = false;
        
        // 增加延迟，确保资源完全释放
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    
    // 保存文件路径
    memset(file_path, 0, sizeof(file_path));
    strncpy(file_path, filepath, sizeof(file_path) - 1);
    
    // 开始播放
    ESP_LOGI(TAG, "开始播放: %s", filepath);
    esp_err_t ret = avi_player_play_from_file(filepath);
    if (ret == ESP_OK) {
        is_playing = true;
        ESP_LOGI(TAG, "播放成功启动");
    } else {
        ESP_LOGE(TAG, "播放启动失败: %s (错误码: %d)", esp_err_to_name(ret), ret);
    }
    
    xSemaphoreGive(avi_mutex);
    return ret;
}

esp_err_t avi_player_port_stop(void)
{
    if (avi_mutex != NULL && xSemaphoreTake(avi_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        if (!is_playing) {
            ESP_LOGW(TAG, "已经停止播放，无需再次停止");
            xSemaphoreGive(avi_mutex);
            return ESP_OK;
        }
        
        ESP_LOGI(TAG, "停止播放");
        esp_err_t ret = avi_player_play_stop();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "停止播放失败: %s", esp_err_to_name(ret));
        }
        
        is_playing = false;
        xSemaphoreGive(avi_mutex);
        return ret;
    } else {
        ESP_LOGE(TAG, "无法获取互斥锁，停止播放操作可能不安全");
        
        // 即使无法获取锁，也尝试停止播放
        if (is_playing) {
            esp_err_t ret = avi_player_play_stop();
            is_playing = false;
            return ret;
        }
        return ESP_OK;
    }
}

void avi_player_port_deinit(void)
{
    avi_player_port_stop();
    avi_player_deinit();
    
    // 删除互斥锁
    if (avi_mutex != NULL) {
        vSemaphoreDelete(avi_mutex);
        avi_mutex = NULL;
    }
    
    if (img_rgb565)
    {
        free(img_rgb565);
        img_rgb565 = NULL;
    }
}