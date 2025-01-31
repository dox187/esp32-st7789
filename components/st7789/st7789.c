#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <driver/ledc.h>

#include "st7789.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "esp_rom_gpio.h"


//config parser
#if CONFIG_SPI2_HOST
	#define SPI_HOST_ID SPI2_HOST
#elif CONFIG_SPI3_HOST
	#define SPI_HOST_ID SPI3_HOST
#endif

#if CONFIG_SPI_DMA_DISABLED
	#define SPI_DMA_CH SPI_DMA_DISABLED
#elif CONFIG_SPI_DMA_CH1
	#define SPI_DMA_CH SPI_DMA_CH1
#elif CONFIG_SPI_DMA_CH2
	#define SPI_DMA_CH SPI_DMA_CH2
#elif CONFIG_SPI_DMA_CH_AUTO
	#define SPI_DMA_CH SPI_DMA_CH_AUTO
#endif

#if CONFIG_SPI_MASTER_FREQ_8M
	#define SPI_MASTER_FREQ SPI_MASTER_FREQ_8M
#elif CONFIG_SPI_MASTER_FREQ_10M 
	#define SPI_MASTER_FREQ SPI_MASTER_FREQ_10M 
#elif CONFIG_SPI_MASTER_FREQ_16M 
	#define SPI_MASTER_FREQ SPI_MASTER_FREQ_16M 
#elif CONFIG_SPI_MASTER_FREQ_20M
	#define SPI_MASTER_FREQ SPI_MASTER_FREQ_20M
#elif CONFIG_SPI_MASTER_FREQ_26M 
	#define SPI_MASTER_FREQ SPI_MASTER_FREQ_26M 
#elif CONFIG_SPI_MASTER_FREQ_40M 
	#define SPI_MASTER_FREQ SPI_MASTER_FREQ_40M 
#elif CONFIG_SPI_MASTER_FREQ_80M
	#define SPI_MASTER_FREQ SPI_MASTER_FREQ_80M
#endif

const char *TAG = "st7789";
ledc_channel_config_t ledc_channel; //for dimmable BL
st7789_driver_t display; //due to availability

static void st7789_pre_cb(spi_transaction_t *transaction)
{
	const st7789_transaction_data_t *data = (st7789_transaction_data_t *)transaction->user;
	gpio_set_level(data->driver->pin_dc, data->data);
}

// backlight
esp_err_t backlight_init(st7789_driver_t *driver){
	esp_err_t err = ESP_OK;
	
	if (driver->pin_backlight != -1)
	{
		if(!CONFIG_ST7789_BL_DIMMABLE){
			esp_rom_gpio_pad_select_gpio(driver->pin_backlight);
		}

		err = gpio_set_direction(driver->pin_dc, GPIO_MODE_OUTPUT);
		if(err != ESP_OK) return err;
		gpio_set_level(driver->pin_backlight, 0);

		if(CONFIG_ST7789_BL_DIMMABLE){
			esp_err_t ledc_timer_config(const ledc_timer_config_t *timer_conf);
			ledc_timer_config_t ledc_timer = {
				.speed_mode       = LEDC_LOW_SPEED_MODE,
				.timer_num        = LEDC_TIMER_0,
				.duty_resolution  = LEDC_TIMER_8_BIT,
				.freq_hz          = 500,
				.clk_cfg          = LEDC_AUTO_CLK
			};
			ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));
			esp_err_t ledc_channel_config(const ledc_channel_config_t *ledc_conf);
			ledc_channel_config_t ledc_channel = {
				.speed_mode     = LEDC_LOW_SPEED_MODE,
				.channel        = LEDC_CHANNEL_0,
				.timer_sel      = LEDC_TIMER_0,
				.intr_type      = LEDC_INTR_FADE_END,
				.gpio_num       = driver->pin_backlight,
				.duty           = 0,
				.hpoint         = 0
			};
			err = ledc_channel_config(&ledc_channel);
			if(err != ESP_OK) return err;
			err = ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 255);
			if(err != ESP_OK) return err;
		}
		
		gpio_set_level(driver->pin_backlight, 1);
	}
	return err;
}

static esp_err_t spi_init(st7789_driver_t *driver)
{
	driver->buffer = (st7789_color_t *)heap_caps_malloc(driver->buffer_size * 2 * sizeof(st7789_color_t), MALLOC_CAP_DMA);
	if (driver->buffer == NULL)
	{
		ESP_LOGE(TAG, "buffer not allocated");
		return ESP_FAIL;
	}
	driver->buffer_a = driver->buffer;
	driver->buffer_b = driver->buffer + driver->buffer_size;
	driver->current_buffer = driver->buffer_a;
	driver->queue_fill = 0;

	driver->data.driver = driver;
	driver->data.data = true;
	driver->command.driver = driver;
	driver->command.data = false;

	esp_rom_gpio_pad_select_gpio(driver->pin_reset);
	esp_rom_gpio_pad_select_gpio(driver->pin_dc);
	gpio_set_direction(driver->pin_reset, GPIO_MODE_OUTPUT);    
	gpio_set_direction(driver->pin_dc, GPIO_MODE_OUTPUT);
	
	if (driver->pin_cs != -1)
	{
		esp_rom_gpio_pad_select_gpio(driver->pin_cs);
		gpio_set_direction(driver->pin_cs, GPIO_MODE_OUTPUT);
		gpio_set_level(driver->pin_cs, 0);
	}
	

	spi_bus_config_t buscfg = {
		.mosi_io_num = driver->pin_mosi,
		.miso_io_num = -1,
		.sclk_io_num = driver->pin_sclk,
		.quadwp_io_num = -1,
		.quadhd_io_num = -1,
		.max_transfer_sz = driver->buffer_size * 2 * sizeof(st7789_color_t), // 2 buffers with 2 bytes for pixel
		.flags = 0, //SPICOMMON_BUSFLAG_NATIVE_PINS, //enable if u need 
	};
	spi_device_interface_config_t devcfg = {
		.clock_speed_hz = SPI_MASTER_FREQ,
		.mode = 0, // https://www.webpages.uidaho.edu/~jfrenzel/340/Handouts/communications/SPI/SPI%20Modes.pdf
		.spics_io_num = driver->pin_cs,
		.queue_size = CONFIG_ST7789_SPI_QUEUE_SIZE,
		.pre_cb = st7789_pre_cb,
		.flags = SPI_DEVICE_NO_DUMMY,
	};

	if (spi_bus_initialize(driver->spi_host, &buscfg, driver->dma_chan) != ESP_OK)
	{
		ESP_LOGE(TAG, "spi bus initialize failed");
		return ESP_FAIL;
	}
	if (spi_bus_add_device(driver->spi_host, &devcfg, &driver->spi) != ESP_OK)
	{
		ESP_LOGE(TAG, "spi bus add device failed");
		return ESP_FAIL;
	}
	ESP_LOGI(TAG, "spi driver initialized");
	return ESP_OK;
}




st7789_driver_t* st7789_init(){
	uint16_t width, height;
	uint16_t offsetx, offsety;
	if(CONFIG_ST7789_ROTATION%2){
		width = CONFIG_ST7789_HEIGHT;
		height = CONFIG_ST7789_WIDTH;
		offsetx = CONFIG_ST7789_OFFSETY;
		offsety = CONFIG_ST7789_OFFSETX;
	}else{
		width = CONFIG_ST7789_WIDTH;
		height = CONFIG_ST7789_HEIGHT;
		offsetx = CONFIG_ST7789_OFFSETX;
		offsety = CONFIG_ST7789_OFFSETY;
	}

	display = (st7789_driver_t){
        .pin_mosi = CONFIG_ST7789_MOSI_GPIO,
        .pin_sclk = CONFIG_ST7789_SCLK_GPIO,
        .pin_cs = CONFIG_ST7789_CS_GPIO,
        .pin_dc = CONFIG_ST7789_DC_GPIO,
        .pin_reset = CONFIG_ST7789_RESET_GPIO,
        .pin_backlight = CONFIG_ST7789_BL_GPIO,
        .spi_host = SPI_HOST_ID,
        .dma_chan = SPI_DMA_CH,
        .display_width = width,
        .display_height = height,
		.offset_x = offsetx,
		.offset_y = offsety,
		.display_rotation = CONFIG_ST7789_ROTATION,
        .buffer_size = CONFIG_ST7789_BUFFER_SIZE * width, // 2 buffers with 20 lines
		.buffer_line_height = CONFIG_ST7789_BUFFER_SIZE,
	};
	ESP_LOGI(TAG, "display driver init _width:%d _height:%d buffer_size:%d",width,height,display.buffer_size);

	ESP_ERROR_CHECK(spi_init(&display));

	
	ESP_ERROR_CHECK(backlight_init(&display));
	
	st7789_reset(&display);
	st7789_lcd_init(&display, CONFIG_ST7789_ROTATION);

	st7789_set_backlight_level(&display, 255);

	return &display;
}

void st7789_reset(st7789_driver_t *driver)
{
	gpio_set_level(driver->pin_reset, 0);
	vTaskDelay(20 / portTICK_PERIOD_MS);
	gpio_set_level(driver->pin_reset, 1);
	vTaskDelay(130 / portTICK_PERIOD_MS);
}

void st7789_lcd_init(st7789_driver_t *driver, uint8_t rotation)
{

	uint8_t madctl = 0;
	switch (rotation)
	{
	case 0:
		break;
	case 1:
		madctl = ST7789_MADCTL_MX | ST7789_MADCTL_MV;
		break;
	case 2:
		madctl = ST7789_MADCTL_MX | ST7789_MADCTL_MY;
		break;
	case 3:
		madctl = ST7789_MADCTL_MY | ST7789_MADCTL_MV;
		break;
	}

	const uint8_t caset[4] = {
		(driver->offset_x) >> 8,
		(driver->offset_x) & 0xff,
		(driver->display_width + driver->offset_x - 1) >> 8,
		(driver->display_width + driver->offset_x - 1) & 0xff};
	const uint8_t raset[4] = {
		(driver->offset_y) >> 8,
		(driver->offset_y) & 0xff,
		(driver->display_height + driver->offset_y - 1) >> 8,
		(driver->display_height + driver->offset_y - 1) & 0xff};
	const st7789_command_t init_sequence[] = {
		// Sleep
		{ST7789_CMD_SLPIN, 10, 0, NULL},	// Sleep
		{ST7789_CMD_SWRESET, 200, 0, NULL}, // Reset
		{ST7789_CMD_SLPOUT, 120, 0, NULL},	// Sleep out

		{ST7789_CMD_MADCTL, 0, 1, &madctl}, // Page / column address order
		{ST7789_CMD_COLMOD, 0, 1, (const uint8_t *)"\x55"}, // 16 bit RGB
		#ifndef CONFIG_ST7789_INVERSION
		{ST7789_CMD_INVON, 0, 0, NULL}, // Inversion off
		#endif
		{ST7789_CMD_CASET, 0, 4, (const uint8_t *)&caset},	// Set width
		{ST7789_CMD_RASET, 0, 4, (const uint8_t *)&raset},	// Set height
		{ST7789_CMD_NORON, 0, 0, NULL}, // Normal mode, Partial off
		// Porch setting
		{ST7789_CMD_PORCTRL, 0, 5, (const uint8_t *)"\x0c\x0c\x00\x33\x33"}, //default
		// Set VGH to 12.54V and VGL to -9.6V
		{ST7789_CMD_GCTRL, 0, 1, (const uint8_t *)"\x14"},
		// Set VCOM to 1.475V
		{ST7789_CMD_VCOMS, 0, 1, (const uint8_t *)"\x37"},
		// Enable VDV/VRH control
		{ST7789_CMD_VDVVRHEN, 0, 2, (const uint8_t *)"\x01\xff"},
		// VAP(GVDD) = 4.45+(vcom+vcom offset+vdv)
		{ST7789_CMD_VRHSET, 0, 1, (const uint8_t *)"\x12"},
		// VDV = 0V
		{ST7789_CMD_VDVSET, 0, 1, (const uint8_t *)"\x20"},
		// AVDD=6.8V, AVCL=-4.8V, VDDS=2.3V
		{ST7789_CMD_PWCTRL1, 0, 2, (const uint8_t *)"\xa4\xa1"},
		// 60 fps
		{ST7789_CMD_FRCTR2, 0, 1, (const uint8_t *)"\x0f"},
		// Gama 2.2
		//{ST7789_CMD_GAMSET, 0, 1, (const uint8_t *)"\x01"},
		// Gama curve
		//{ST7789_CMD_PVGAMCTRL, 0, 14, (const uint8_t *)"\xd0\x08\x11\x08\x0c\x15\x39\x33\x50\x36\x13\x14\x29\x2d"},
		//{ST7789_CMD_NVGAMCTRL, 0, 14, (const uint8_t *)"\xd0\x08\x10\x08\x06\x06\x39\x44\x51\x0b\x16\x14\x2f\x31"},

		// Little endian
		// Note: Little Endian only can be supported in 65K 8-bit and 9-bit interface. 
		// 1-4 datapin spi is not supported
		{ST7789_CMD_RAMCTRL, 0, 2, (const uint8_t *)"\x00\xc8"},
		{ST7789_CMDLIST_END, 0, 0, NULL}, // End of commands
	};
	st7789_run_commands(driver, init_sequence);
	st7789_clear(driver, 0x0000);
	const st7789_command_t init_sequence2[] = {
		{ST7789_CMD_DISPON, 100, 0, NULL}, // Display on
		{ST7789_CMD_SLPOUT, 100, 0, NULL}, // Sleep out
		{ST7789_CMD_CASET, 0, 4, caset},
		{ST7789_CMD_RASET, 0, 4, raset},
		{ST7789_CMD_RAMWR, 0, 0, NULL},
		{ST7789_CMDLIST_END, 0, 0, NULL}, // End of commands
	};
	st7789_run_commands(driver, init_sequence2);
}

void st7789_set_backlight(st7789_driver_t *driver, bool enable)
{
	if (driver->pin_backlight != -1)
	{
		gpio_set_level(driver->pin_backlight, enable);
	}
}

void st7789_set_backlight_level(st7789_driver_t *driver, uint8_t level)
{
	if (driver->pin_backlight != -1)
	{
		if(CONFIG_ST7789_BL_DIMMABLE)
		{
			ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));
			ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, level));
		}
		else
		{
			if(level > 16) 
			{
				st7789_set_backlight(driver, true);
			} 
			else 
			{
				st7789_set_backlight(driver, false);
			}
		}
	}
}

void st7789_start_command(st7789_driver_t *driver)
{
	gpio_set_level(driver->pin_dc, 0);
}

void st7789_start_data(st7789_driver_t *driver)
{
	gpio_set_level(driver->pin_dc, 1);
}

void st7789_run_command(st7789_driver_t *driver, const st7789_command_t *command)
{
	spi_transaction_t *rtrans;
	st7789_wait_until_queue_empty(driver);
	spi_transaction_t trans;
	memset(&trans, 0, sizeof(trans));
	trans.length = 8; // 8 bits
	trans.tx_buffer = &command->command;
	trans.user = &driver->command;
	spi_device_queue_trans(driver->spi, &trans, portMAX_DELAY);
	spi_device_get_trans_result(driver->spi, &rtrans, portMAX_DELAY);

	if (command->data_size > 0)
	{
		memset(&trans, 0, sizeof(trans));
		trans.length = command->data_size * 8;
		trans.tx_buffer = command->data;
		trans.user = &driver->data;
		spi_device_queue_trans(driver->spi, &trans, portMAX_DELAY);
		spi_device_get_trans_result(driver->spi, &rtrans, portMAX_DELAY);
	}

	if (command->wait_ms > 0)
	{
		vTaskDelay(command->wait_ms / portTICK_PERIOD_MS);
	}
}

void st7789_run_commands(st7789_driver_t *driver, const st7789_command_t *sequence)
{
	while (sequence->command != ST7789_CMDLIST_END)
	{
		st7789_run_command(driver, sequence);
		sequence++;
	}
}

void st7789_clear(st7789_driver_t *driver, st7789_color_t color)
{
	st7789_fill_area(driver, color, 0, 0, driver->display_width, driver->display_height);
}

void st7789_fill_area(st7789_driver_t *driver, st7789_color_t color, uint16_t start_x, uint16_t start_y, uint16_t width, uint16_t height)
{
	for (size_t i = 0; i < driver->buffer_size * 2; ++i)
	{
		driver->buffer[i] = color;
	}
	st7789_set_window(driver, start_x, start_y, start_x + width - 1, start_y + height - 1);

	size_t bytes_to_write = width * height * 2;
	size_t transfer_size = driver->buffer_size * 2 * sizeof(st7789_color_t);

	spi_transaction_t trans;
	memset(&trans, 0, sizeof(trans));
	trans.tx_buffer = driver->buffer;
	trans.user = &driver->data;
	trans.length = transfer_size * 8;
	trans.rxlength = 0;

	spi_transaction_t *rtrans;

	while (bytes_to_write > 0)
	{
		if (driver->queue_fill >= CONFIG_ST7789_SPI_QUEUE_SIZE)
		{
			spi_device_get_trans_result(driver->spi, &rtrans, portMAX_DELAY);
			driver->queue_fill--;
		}
		if (bytes_to_write < transfer_size)
		{
			transfer_size = bytes_to_write;
		}
		spi_device_queue_trans(driver->spi, &trans, portMAX_DELAY);
		driver->queue_fill++;
		bytes_to_write -= transfer_size;
	}

	st7789_wait_until_queue_empty(driver);
}

void st7789_set_window(st7789_driver_t *driver, uint16_t start_x, uint16_t start_y, uint16_t end_x, uint16_t end_y)
{
	start_x	+= driver->offset_x;
	end_x	+= driver->offset_x;
	start_y	+= driver->offset_y;
	end_y	+= driver->offset_y;
	uint8_t caset[4];
	uint8_t raset[4];
	caset[0] = (uint8_t)(start_x >> 8);
	caset[1] = (uint8_t)(start_x & 0xff);
	caset[2] = (uint8_t)(end_x >> 8);
	caset[3] = (uint8_t)(end_x & 0xff);
	raset[0] = (uint8_t)(start_y >> 8);
	raset[1] = (uint8_t)(start_y & 0xff);
	raset[2] = (uint8_t)(end_y >> 8);
	raset[3] = (uint8_t)(end_y & 0xff);
	st7789_command_t sequence[] = {
		{ST7789_CMD_CASET, 0, 4, caset},
		{ST7789_CMD_RASET, 0, 4, raset},
		{ST7789_CMD_RAMWR, 0, 0, NULL},
		{ST7789_CMDLIST_END, 0, 0, NULL},
	};
	st7789_run_commands(driver, sequence);
}

//todo this is a _write_pixels_from_active_buffer
void st7789_write_pixels(st7789_driver_t *driver, st7789_color_t *pixels, size_t length)
{
	st7789_wait_until_queue_empty(driver);

	spi_transaction_t *trans = driver->current_buffer == driver->buffer_a ? &driver->trans_a : &driver->trans_b;
	memset(trans, 0, sizeof(&trans));
	trans->tx_buffer = driver->current_buffer;
	trans->user = &driver->data;
	trans->length = length * sizeof(st7789_color_t) * 8;
	trans->rxlength = 0;

	spi_device_queue_trans(driver->spi, trans, portMAX_DELAY);
	driver->queue_fill++;
}

void st7789_wait_until_queue_empty(st7789_driver_t *driver)
{
	spi_transaction_t *rtrans;
	while (driver->queue_fill > 0)
	{
		spi_device_get_trans_result(driver->spi, &rtrans, portMAX_DELAY);
		driver->queue_fill--;
	}
}

void st7789_swap_buffers(st7789_driver_t *driver)
{
	st7789_write_pixels(driver, driver->current_buffer, driver->buffer_size);
	driver->current_buffer = driver->current_buffer == driver->buffer_a ? driver->buffer_b : driver->buffer_a;
}

uint8_t st7789_dither_table[256] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

void st7789_randomize_dither_table()
{
	uint16_t *dither_table = (uint16_t *)st7789_dither_table;
	for (size_t i = 0; i < sizeof(st7789_dither_table) / 2; ++i)
	{
		dither_table[i] = rand() & 0xffff;
	}
}

void st7789_draw_gray2_bitmap(uint8_t *src_buf, st7789_color_t *target_buf, uint8_t r, uint8_t g, uint8_t b, int x, int y, int src_w, int src_h, int target_w, int target_h)
{
	if (x >= target_w || y >= target_h || x + src_w <= 0 || y + src_h <= 0)
	{
		return;
	}

	const size_t src_size = src_w * src_h;
	const size_t target_size = target_w * target_h;
	const size_t line_w = MIN(src_w + x, target_w) - MAX(x, 0);
	const size_t src_skip = src_w - line_w;
	const size_t target_skip = target_w - line_w;
	size_t src_pos = 0;
	size_t target_pos = 0;
	size_t x_pos = 0;
	size_t y_pos = 0;

	if (y < 0)
	{
		src_pos = (-y) * src_w;
	}
	if (x < 0)
	{
		src_pos -= x;
	}
	if (y > 0)
	{
		target_pos = y * target_w;
	}
	if (x > 0)
	{
		target_pos += x;
	}

	while (src_pos < src_size && target_pos < target_size)
	{
		uint8_t src_r, src_g, src_b;
		uint8_t target_r, target_g, target_b;
		st7789_color_to_rgb(target_buf[target_pos], &src_r, &src_g, &src_b);
		uint8_t gray2_color = (src_buf[src_pos >> 2] >> ((src_pos & 0x03) << 1)) & 0x03;
		/*
		static const uint32_t src_weights = 0x002b5580;
		static const uint32_t target_weights = 0x80552b00;
		const uint32_t src_weight = (src_weights >> (gray2_color << 3)) & 0xff;
		const uint32_t target_weight = (target_weights >> (gray2_color << 3)) & 0xff;
		target_r = ((src_weight * src_r) + (target_weight * r)) >> 7;
		target_g = ((src_weight * src_g) + (target_weight * g)) >> 7;
		target_b = ((src_weight * src_b) + (target_weight * b)) >> 7;
		target_buf[target_pos] = st7789_rgb_to_color_dither(target_r, target_g, target_b, x_pos, y_pos);
		*/
		switch (gray2_color)
		{
		case 1:
			target_r = r >> 1;
			target_g = g >> 1;
			target_b = b >> 1;
			src_r = (src_r >> 1) + target_r;
			src_g = (src_g >> 1) + target_g;
			src_b = (src_b >> 1) + target_b;
			target_buf[target_pos] = st7789_rgb_to_color_dither(src_r, src_g, src_b, x_pos, y_pos);
			break;
		case 2:
			target_r = r >> 2;
			target_g = g >> 2;
			target_b = b >> 2;
			src_r = (src_r >> 2) + target_r + target_r + target_r;
			src_g = (src_g >> 2) + target_g + target_g + target_g;
			src_b = (src_b >> 2) + target_b + target_b + target_b;
			target_buf[target_pos] = st7789_rgb_to_color_dither(src_r, src_g, src_b, x_pos, y_pos);
			break;
		case 3:
			target_buf[target_pos] = st7789_rgb_to_color_dither(r, g, b, x_pos, y_pos);
			break;
		default:
			break;
		}

		x_pos++;

		if (x_pos == line_w)
		{
			x_pos = 0;
			y_pos++;
			src_pos += src_skip;
			target_pos += target_skip;
		}
		src_pos++;
		target_pos++;
	}
}
