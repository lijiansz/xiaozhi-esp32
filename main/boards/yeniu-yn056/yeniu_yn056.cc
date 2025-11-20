#include "wifi_board.h"
#include "codecs/es8311_audio_codec.h"
#include "display/lcd_display.h"
#include "system_reset.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "mcp_server.h"
#include "led/single_led.h"
#include "assets/lang_config.h"

#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <driver/i2c_master.h>
#include <wifi_station.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <esp_timer.h>

#include "power_manager.h"
#include "power_save_timer.h"
#include <driver/rtc_io.h>

#define TAG "yn"
#define BATTERY_ADC_CHANNEL ADC_CHANNEL_0
#define BATTERY_ADC_ATTEN ADC_ATTEN_DB_12
#define BATTERY_READ_INTERVAL_MS (10 * 1000) // 10 �?

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_awesome_20_4);

class yeniu_yn052 : public WifiBoard
{
private:
    Button boot_button_;
    Button volume_up_button_;
    Button volume_down_button_;
    Button power_button_;
    LcdDisplay *display_;
    i2c_master_bus_handle_t codec_i2c_bus_;

    PowerSaveTimer* power_save_timer_;
    PowerManager* power_manager_;

    int power_satte = 0;    // =0: 关机状�?? --> �?�?; =1: �?机状�? --> 关机
    void InitializePowerManager() {
        power_manager_ = new PowerManager(GPIO_NUM_7);      //充电状�?�脚
        // power_manager_->OnTemperatureChanged([this](float chip_temp) {
        //     //display_->UpdateHighTempWarning(chip_temp);
        // });

        power_manager_->OnChargingStatusChanged([this](bool is_charging) {
            if (is_charging) {
                power_save_timer_->SetEnabled(false);
                ESP_LOGI("PowerManager", "Charging started");
            } else {
                power_save_timer_->SetEnabled(true);
                ESP_LOGI("PowerManager", "Charging stopped");
            }
        });    
    }

    void InitializePowerSaveTimer() {
        rtc_gpio_init(GPIO_NUM_2);
        rtc_gpio_set_direction(GPIO_NUM_2, RTC_GPIO_MODE_OUTPUT_ONLY);
        rtc_gpio_set_level(GPIO_NUM_2, 1);

        power_save_timer_ = new PowerSaveTimer(-1, 60, 300);
        power_save_timer_->OnEnterSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(true);
            GetBacklight()->SetBrightness(1);
        });
        power_save_timer_->OnExitSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(false);
            GetBacklight()->RestoreBrightness();
        });
        power_save_timer_->SetEnabled(true);
    }

    void InitializeCodecI2c()
    {
        // 初始�? I2C 外设
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &codec_i2c_bus_));
    }

    void InitializeSpi()
    {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_SPI_MOSI_PIN;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_SPI_SCK_PIN;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeButtons()
    {
        boot_button_.OnClick([this]()
        {
            auto& app = Application::GetInstance();
            ESP_LOGI(TAG, "[ZG] boot_button_ OnClick %d",app.GetDeviceState());
            if (app.GetDeviceState() == kDeviceStateStarting && !WifiStation::GetInstance().IsConnected()) {
                ResetWifiConfiguration();
            }
        });
        
        boot_button_.OnPressDown([this]()
        { 
            Application::GetInstance().StartListening(); 
        });
        
        boot_button_.OnPressUp([this]()
        {
            Application::GetInstance().StopListening(); 
        });
        
        // 电源按键，长�?
        
        power_button_.OnLongPress([this]()
        {
            ESP_LOGI(TAG, "[ZG] power_button_.OnLongPress state: %d", power_satte);
            if (power_satte == 0)
            {
                gpio_set_level(POWER_CTL_GPIO, 1); // �?�?
                power_satte = 1;
            }
            else
            {
                auto& app = Application::GetInstance();
                app.Alert(Lang::Strings::INFO, "OK", "sad", Lang::Sounds::OGG_SUCCESS);
                vTaskDelay(pdMS_TO_TICKS(600)); // 等待提示音播放完�?
                gpio_set_level(POWER_CTL_GPIO, 0); // 关机
                power_satte = 0;
            }
        });

        
        // 电源按键，双击（配网�?
        power_button_.OnDoubleClick([this]()
        {
            ResetWifiConfiguration();
        });
        

        volume_up_button_.OnClick([this]()
        {
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() + 10;
            if (volume > 100) {
                volume = 100;
            }
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume)); 
        });

        volume_up_button_.OnLongPress([this]()
        {
            GetAudioCodec()->SetOutputVolume(100);
            GetDisplay()->ShowNotification(Lang::Strings::MAX_VOLUME); 
        });

        volume_down_button_.OnClick([this]()
        {
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() - 10;
            if (volume < 0) {
                volume = 0;
            }
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume)); 
        });

        volume_down_button_.OnLongPress([this]()
        {
            GetAudioCodec()->SetOutputVolume(0);
            GetDisplay()->ShowNotification(Lang::Strings::MUTED); 
        });
    }

    void InitializeSt7789Display()
    {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        // 液晶屏控制IO初始�?
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_SPI_CS_PIN;
        io_config.dc_gpio_num = DISPLAY_DC_PIN;
        io_config.spi_mode = 3;
        io_config.pclk_hz = 80 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

        // 初始化液晶屏驱动芯片 ST7789
        ESP_LOGD(TAG, "Install LCD driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RES;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
        ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY));
        ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y));
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, true));

        display_ = new SpiLcdDisplay(panel_io, panel,
                                     DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY
    //                                 ,{
    //                                     .text_font = &font_puhui_20_4,
    //                                     .icon_font = &font_awesome_20_4,
    //                                     .emoji_font = font_emoji_64_init(),
    //                                 }
                                     );
    }

    void InitializeTools()
    {
        // 可扩展工具初始化
    }


public:
    yeniu_yn052() :  
        boot_button_(BOOT_BUTTON_GPIO),
        volume_up_button_(VOLUME_UP_BUTTON_GPIO),
        volume_down_button_(VOLUME_DOWN_BUTTON_GPIO),
        power_button_(POWER_KEY_BUTTON_GPIO)
    {
        ESP_LOGI(TAG, "Initializing yeniu_yn052 Board");

        // 初始化板载LED
        gpio_config_t led_config = {
            .pin_bit_mask = 1ULL << BUILTIN_LED_GPIO,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&led_config));

        InitializePowerManager();
        InitializePowerSaveTimer();

        InitializeCodecI2c();
        InitializeSpi();
        InitializeButtons();
        InitializeSt7789Display();
        InitializeTools();

        GetBacklight()->RestoreBrightness();
    }

    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        static bool last_discharging = false;
        charging = power_manager_->IsCharging();
        discharging = power_manager_->IsDischarging();
        if (discharging != last_discharging) {
            power_save_timer_->SetEnabled(discharging);
            last_discharging = discharging;
        }
        // level = std::max<uint32_t>(power_manager_->GetBatteryLevel(), 20);
        level = power_manager_->GetBatteryLevel();
        // ESP_LOGI("PowerManager", "GetBatteryLevel %d",(int)charging);
        return true;
    }

    ~yeniu_yn052()
    {
        ;
    }

    virtual AudioCodec *GetAudioCodec() override
    {
        static Es8311AudioCodec audio_codec(codec_i2c_bus_, I2C_NUM_0, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
                                            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
                                            AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR, true);
        return &audio_codec;
    }

    virtual Display *GetDisplay() override
    {
        return display_;
    }

    virtual Backlight *GetBacklight() override
    {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }
};

// 注册�?发板
DECLARE_BOARD(yeniu_yn052);