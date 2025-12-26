#pragma once

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief 启动 BluFi 应用程序
     *
     * @note 初始化并启动蓝牙配网服务
     */
    void blufi_app_start(void);

    /**
     * @brief 停止 BluFi 应用程序
     *
     * @note 停止蓝牙配网服务并释放相关资源
     */
    void blufi_app_stop(void);

#ifdef __cplusplus
}
#endif
