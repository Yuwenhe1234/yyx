#ifndef DAC_H
#define DAC_H

#include "esp_err.h"

// DAC 配置
#define DAC_CHANNEL DAC_CHAN_0  // GPIO25

// 输出类型
typedef enum {
    DAC_OUTPUT_DC,
    DAC_OUTPUT_SINE,
    DAC_OUTPUT_PULSE
} dac_output_type_t;

// 初始化 DAC
esp_err_t dac_init(void);

// 设置输出类型
esp_err_t dac_set_output(dac_output_type_t type, float param1, float param2);

// 停止输出
esp_err_t dac_stop(void);

// 内部任务处理函数 (不直接调用)
void dac_task(void *param);

#endif // DAC_H