/*
 ============================================================================
 Name        : touch-test.c
 Author      : Nick v. IJzendoorn
 Version     :
 Copyright   : Confed Solutions
 Description : Hello World in C, Ansi-style
 ============================================================================
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/ioctl.h>

#include <gpiod.h>
#include <linux/ioctl.h>
#include <linux/types.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>

#define MIN(a, b)					((a) > (b) ? (b) : (a))

#define ARRAY_SIZEOF(x)				(sizeof(x) / (sizeof(x[0])))

#define CF1124_ADDRESS				0x55
#define CF1124_MAX_FINGERS			2

#define REG_FW_VERSION				0x00
#define REG_FW_REVISION				0x0c
#define REG_FINGERS					0x10
#define REG_MAX_NUM_TOUCHES			0x3f
#define REG_MISC_CONTROL			0xf1
#define REG_SMART_WAKEUP			0xf2
#define REG_CHIP_ID					0xf4

#define LINE_RESET					6
#define LINE_IRQ					27
#define GPIOD_EVENT_BUFFER_SIZE		1

static struct gpiod_line_request* _gpiod_request_output_line(const char *path, uint32_t offset, bool active_low, enum gpiod_line_value value, const char *consumer);
static struct gpiod_line_request* _gpiod_request_input_line(const char *path, uint32_t offset, bool active_low, const char *consumer);
static int _i2c_open(const char *dev_path, uint8_t address);
static int _touch_do_initialize(int i2c_fd, struct gpiod_line_request *io_reset);

static int _i2c_write_reg(int fd, uint8_t reg, uint8_t *data, size_t data_len);
static int _i2c_read_reg(int fd, uint8_t reg, uint8_t *data, size_t data_len);

int main(int argc, char **argv)
{
	// parse the command line options
	int poll_interval_ms = 10;
	int do_irq_watch = 0;
	int do_gestures = 0;

	int opt = 0;
	while ((opt = getopt(argc, argv, "p:ig")) != -1)
	{
		switch (opt)
		{
		case 'p':
		{
			// parse the poll_interval_ms value
			char *end;
			poll_interval_ms = strtol(optarg, &end, 10);
			char *expected_end = optarg + strlen(optarg);
			if (expected_end != end)
				goto print_usage;
		} break;

		case 'i':
			do_irq_watch = 1;
			break;

		case 'g':
			do_gestures = 1;
			break;

		default:
			goto print_usage;
		}
	}

	// get the GPIO chip and lines
	printf("open the touch RESET and IQRn GPIO lines\r\n");
	struct gpiod_line_request* io_reset = _gpiod_request_output_line("/dev/gpiochip1", LINE_RESET, true, GPIOD_LINE_VALUE_INACTIVE, "touch_reset");
	struct gpiod_line_request* io_irq = _gpiod_request_input_line("/dev/gpiochip1", LINE_IRQ, true, "touch_irq");
	if (! io_reset || ! io_irq)
		return -1;

	// open the I2C bus
	printf("open the touch I2C bus\r\n");
	int i2c_fd = _i2c_open("/dev/i2c-0", CF1124_ADDRESS);
	if (i2c_fd < 0)
		return -1;

	// reset and initialize the touch controller
	if (_touch_do_initialize(i2c_fd, io_reset) < 0)
		return -1;

	// prepare for IRQ readout if requested
	struct gpiod_edge_event_buffer *event_buffer = NULL;
	struct gpiod_edge_event *event = NULL;
	if (do_irq_watch)
	{
		event_buffer = gpiod_edge_event_buffer_new(GPIOD_EVENT_BUFFER_SIZE);
		if (! event_buffer)
		{
			perror("gpiod_edge_event_buffer_new()");
			return -1;
		}
	}

	// readout the chip in the requested mode
	uint8_t last_touch[CF1124_MAX_FINGERS];
	while (true)
	{
		// use IRQ watch if requested
		if (do_irq_watch) // TODO add? && gpiod_line_request_get_value(io_irq, LINE_IRQ) != GPIOD_LINE_VALUE_ACTIVE)
		{
			// wait for a falling edge
			int ret = gpiod_line_request_read_edge_events(io_irq, event_buffer, GPIOD_EVENT_BUFFER_SIZE);
			if (ret < 0)
			{
				perror("gpiod_line_request_read_edge_events()");
				return -1;
			}

			// we detected a falling edge, do a readout
		}

		// check if there was a smart wakeup event
		uint8_t swu;
		if (_i2c_read_reg(i2c_fd, REG_SMART_WAKEUP, &swu, sizeof(swu)) < 0)
		{
			perror("i2c_read_reg(REG_SMART_WAKEUP)");
			return -1;
		}

		if (swu)
		{
			printf(" ! swu: %d\r\n", swu);

			swu = 0;
			if (_i2c_write_reg(i2c_fd, REG_SMART_WAKEUP, &swu, sizeof(swu)) < 0)
			{
				perror("i2c_write_reg(REG_SMART_WAKEUP)");
				return -1;
			}
		}

		// do touch data readout
		uint8_t buffer[2 + (4 * CF1124_MAX_FINGERS)];
		if (_i2c_read_reg(i2c_fd, REG_FINGERS, buffer, sizeof(buffer)) < 0)
		{
			perror("i2c_read_reg(REG_FINGERS)");
			break;
		}

		// check if gesture was made
		if (buffer[0])
			printf(" ! gesture: %d\r\n", buffer[0]);

		// check if a key was pressed
		if (buffer[1])
			printf(" ! key: %d\r\n", buffer[1]);

		// process the multi-touch data
		for (int idx = 0; idx < CF1124_MAX_FINGERS; ++idx)
		{
			// check if a touch is present
			if (buffer[(4 * idx) + 2] & 0x80)
			{
				// readout the touch data
				int32_t x = ((buffer[(4 * idx) + 2] & 0x70) << 4)
								| (buffer[(4 * idx) + 3]);
				int32_t y = ((buffer[(4 * idx) + 2] & 0x0F) << 8)
								| (buffer[(4 * idx) + 4]);

				// rotate the coordinates to reality
				int32_t temp = 320 - y;
				y = x;
				x = temp;

				// mark that a touch is being made
				last_touch[idx] = 1;

				// print our touch info to the console
				printf("touch %d = (x: %ld - y: %ld)\r\n", idx, x, y);
			}
			else if (last_touch[idx])
			{
				printf("touch %d released\r\n", idx);

				last_touch[idx] = 0;
			}
		}

		// sleep for the poll interval if not in IRQ mode readout
		if (! do_irq_watch)
			usleep(poll_interval_ms * 1000);
	}

	// close the I2C bus
	close(i2c_fd);

	// close the GPIO lines and chip
	gpiod_line_request_release(io_reset);
	gpiod_line_request_release(io_irq);

	return EXIT_SUCCESS;

print_usage:
	printf("usage: %s [-p poll_interval_ms] [-i] [-g]\r\n", argv[0]);

	return -EXIT_FAILURE;
}

static int _i2c_open(const char *dev_path, uint8_t address)
{
	// open the SPI bus
	int i2c_fd = open(dev_path, O_RDWR);
	if (i2c_fd < 0)
	{
		perror("open(i2c_dev)");
		return -1;
	}

	// set the slave address
	if (ioctl(i2c_fd, I2C_SLAVE, address) < 0)
	{
		perror("ioctl(I2C_SLAVE)");
		return -1;
	}

	// read the chip ID
	uint8_t chip_id;
	if (_i2c_read_reg(i2c_fd, REG_CHIP_ID, &chip_id, sizeof(chip_id)) < 0)
	{
		perror("i2c_read_reg(REG_CHIP_ID)");
		return -1;
	}

	printf("found CF1124: ID = 0x%02x\r\n", chip_id);

	// read the version information
	uint8_t fw_version;
	if (_i2c_read_reg(i2c_fd, REG_FW_VERSION, &fw_version, sizeof(fw_version)) < 0)
	{
		perror("i2c_read_reg(REG_FW_VERSION)");
		return -1;
	}

	uint8_t fw_revision[4];
	if (_i2c_read_reg(i2c_fd, REG_FW_REVISION, fw_revision, sizeof(fw_revision)) < 0)
	{
		perror("i2c_read_reg(REG_FW_REVISION)");
		return -1;
	}

	// print the firmware information
	printf("firmware version: v%d.%d.%d.%d.%d\r\n", fw_version, fw_revision[3], fw_revision[2], fw_revision[1], fw_revision[0]);

	// read the chip ID
	uint8_t max_touches;
	if (_i2c_read_reg(i2c_fd, REG_MAX_NUM_TOUCHES, &max_touches, sizeof(max_touches)) < 0)
	{
		perror("i2c_read_reg(REG_MAX_NUM_TOUCHES)");
		return -1;
	}

	printf("max touches: %d\r\n", max_touches);

	// enable keys and gestures
	uint8_t misc;
	if (_i2c_read_reg(i2c_fd, REG_MISC_CONTROL, &misc, sizeof(misc)) < 0)
	{
		perror("i2c_read_reg(REG_MISC_CONTORL)");
		return -1;
	}

	// enable the smart wake-up feature
	misc |= 0x80;

	if (_i2c_write_reg(i2c_fd, REG_MISC_CONTROL, &misc, sizeof(misc)) < 0)
	{
		perror("_i2c_write_reg(REG_MISC_CONTROL)");
		return -1;
	}

	printf("enabled keys and gesture recognition\r\n");

	return i2c_fd;
}

static struct gpiod_line_request* _gpiod_request_output_line(const char *path, uint32_t offset, bool active_low, enum gpiod_line_value value, const char *consumer)
{
	struct gpiod_request_config *req_cfg = NULL;
	int ret;

	struct gpiod_chip *chip = gpiod_chip_open(path);
	if (!chip)
		return NULL;

	struct gpiod_line_settings *settings = gpiod_line_settings_new();
	if (!settings)
		goto close_chip;

	gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_OUTPUT);
	gpiod_line_settings_set_output_value(settings, value);
	gpiod_line_settings_set_active_low(settings, active_low);

	struct gpiod_line_config *line_cfg = gpiod_line_config_new();
	if (!line_cfg)
		goto free_settings;

	ret = gpiod_line_config_add_line_settings(line_cfg, &offset, 1, settings);
	if (ret)
		goto free_line_config;

	if (consumer) {
		req_cfg = gpiod_request_config_new();
		if (!req_cfg)
			goto free_line_config;

		gpiod_request_config_set_consumer(req_cfg, consumer);
	}

	struct gpiod_line_request *request = gpiod_chip_request_lines(chip, req_cfg, line_cfg);

	gpiod_request_config_free(req_cfg);

free_line_config:
	gpiod_line_config_free(line_cfg);

free_settings:
	gpiod_line_settings_free(settings);

close_chip:
	gpiod_chip_close(chip);

	return request;
}

static struct gpiod_line_request* _gpiod_request_input_line(const char *path, uint32_t offset, bool active_low, const char *consumer)
{
	struct gpiod_request_config *req_cfg = NULL;
	int ret;

	struct gpiod_chip *chip = gpiod_chip_open(path);
	if (!chip)
		return NULL;

	struct gpiod_line_settings *settings = gpiod_line_settings_new();
	if (!settings)
		goto close_chip;

	gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_INPUT);
	gpiod_line_settings_set_edge_detection(settings, GPIOD_LINE_EDGE_FALLING);
	gpiod_line_settings_set_active_low(settings, active_low);

	struct gpiod_line_config *line_cfg = gpiod_line_config_new();
	if (!line_cfg)
		goto free_settings;

	ret = gpiod_line_config_add_line_settings(line_cfg, &offset, 1, settings);
	if (ret)
		goto free_line_config;

	if (consumer) {
		req_cfg = gpiod_request_config_new();
		if (!req_cfg)
			goto free_line_config;

		gpiod_request_config_set_consumer(req_cfg, consumer);
	}

	struct gpiod_line_request *request = gpiod_chip_request_lines(chip, req_cfg, line_cfg);

	gpiod_request_config_free(req_cfg);

free_line_config:
	gpiod_line_config_free(line_cfg);

free_settings:
	gpiod_line_settings_free(settings);

close_chip:
	gpiod_chip_close(chip);

	return request;
}

static int _touch_do_initialize(int i2c_fd, struct gpiod_line_request *io_reset)
{
	printf("do touch initialization\r\n");

	// configure the touch

	// toggle the touch reset line
	if (gpiod_line_request_set_value(io_reset, LINE_RESET, GPIOD_LINE_VALUE_INACTIVE) < 0)
		return -1;

	usleep(1000);

	if (gpiod_line_request_set_value(io_reset, LINE_RESET, GPIOD_LINE_VALUE_ACTIVE) < 0)
		return -1;

	usleep(100000);

	if (gpiod_line_request_set_value(io_reset, LINE_RESET, GPIOD_LINE_VALUE_INACTIVE) < 0)
		return -1;

	usleep(250000);

	return 0;
}

static int _i2c_write_reg(int fd, uint8_t reg, uint8_t *data, size_t data_len)
{
	// format the data packet
	uint32_t buffer_len = data_len + 1;
	uint8_t buffer[buffer_len];
	buffer[0] = reg;
	memcpy(buffer + 1, data, data_len);

	// write the register and data to the chip
	if (write (fd, buffer, buffer_len) != buffer_len)
		return -1;

	return 0;
}

static int _i2c_read_reg(int fd, uint8_t reg, uint8_t *data, size_t data_len)
{
	// write the register to the chip
	if (write(fd, &reg, sizeof(reg)) != sizeof(reg))
		return -1;

	// read the data from the chip
	if (read(fd, data, data_len) != data_len)
		return -1;

	return 0;
}
