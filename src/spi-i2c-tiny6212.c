/* TINY6212 SPI-I2C Bridge driver

   Used in PinePhone Fingerprint Reader Case.

   Hardware by Zachary Schroeder (https://github.com/zschroeder6212)
   Firmware: https://github.com/zschroeder6212/tiny-i2c-spi

   Written by Egor Vorontsov <sdoregor@sdore.me>
   Based on the mainlined `drivers/spi/spi-sc18is602.c' by Guenter Roeck <linux@roeck-us.net> licensed under GPL.

   Proper licensing will be included with the first release.
*/

#include <linux/of.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/spi/spi.h>

#define TINY6212_BUFSIZ		128
#define TINY6212_CLOCK		8000000

#define TINY6212_MODE_CPOL		BIT(0)
#define TINY6212_MODE_CPHA		BIT(1)
#define TINY6212_MODE_MSB_FIRST		BIT(2)
#define TINY6212_MODE_CLOCK_DIV_2	(0x0 << 3)
#define TINY6212_MODE_CLOCK_DIV_4	(0x1 << 3)
#define TINY6212_MODE_CLOCK_DIV_8	(0x2 << 3)
#define TINY6212_MODE_CLOCK_DIV_16	(0x3 << 3)
#define TINY6212_MODE_CLOCK_DIV_32	(0x4 << 3)
#define TINY6212_MODE_CLOCK_DIV_64	(0x5 << 3)
#define TINY6212_MODE_CLOCK_DIV_128	(0x6 << 3)

#define TINY6212_MODE_DEFAULT	0b100

#define TINY6212_CMD_TRANSMIT	0x1
#define TINY6212_CMD_CONFIGURE	0x2

struct tiny6212 {
	struct spi_master *master;
	struct device *dev;
	u8 ctrl;
	u32 freq;

	struct i2c_client *client;
	u8 rbuf[TINY6212_BUFSIZ+1];
	u8 wbuf[TINY6212_BUFSIZ+1];
	int tlen;
};

static int tiny6212_txrx(struct tiny6212 *hw, struct spi_message *msg, struct spi_transfer *t, bool do_transfer) {
	if (hw->tlen == 0) {
		hw->wbuf[0] = TINY6212_CMD_TRANSMIT;
		hw->tlen = 1;
	}

	if (t->tx_buf) {
		memcpy(&hw->wbuf[hw->tlen], t->tx_buf, t->len);
		hw->tlen += t->len;
		if (t->rx_buf) do_transfer = true;
	}

	if (do_transfer && hw->tlen > 1) {
		int ret = i2c_master_send(hw->client, hw->wbuf, hw->tlen);
		if (ret < 0) return ret;
		if (ret != hw->tlen) return -EIO;
		hw->tlen = 0;
	}

	if (t->rx_buf) {
		int j;
		for (j = 0; j < t->len; j++) {
			int ret = i2c_master_recv(hw->client, &hw->rbuf[j], 1);
			if (ret < 0) return ret;
			if (ret == 0) return -EIO;
		}
		memcpy(t->rx_buf, hw->rbuf, t->len);
	}

	return t->len;
}

static int tiny6212_setup_transfer(struct tiny6212 *hw, u32 hz, u8 mode) {
	u8 ctrl = 0, cmd[] = {TINY6212_CMD_CONFIGURE, 0};
	int ret;

	if (mode & SPI_CPOL) ctrl |= TINY6212_MODE_CPOL;
	if (mode & SPI_CPHA) ctrl |= TINY6212_MODE_CPHA;
	if (!(mode & SPI_LSB_FIRST)) ctrl |= TINY6212_MODE_MSB_FIRST;

	/* Find the closest clock speed */
	#define F(div)	if (hz >= hw->freq/div) ctrl |= TINY6212_MODE_CLOCK_DIV_##div; else
	F(2)
	F(4)
	F(8)
	F(16)
	F(32)
	F(64)
	/* else F(128) */ ctrl |= TINY6212_MODE_CLOCK_DIV_128;
	#undef F

	if (ctrl == hw->ctrl) return 0;

	cmd[1] = ctrl;
	ret = i2c_master_send(hw->client, cmd, sizeof(cmd)/sizeof(*cmd));
	if (ret < 0) return ret;

	hw->ctrl = ctrl;

	return 0;
}

static int tiny6212_check_transfer(struct spi_device *spi, struct spi_transfer *t, int tlen) {
	if (t && t->len-1 + tlen > TINY6212_BUFSIZ) return -EINVAL;

	return 0;
}

static int tiny6212_transfer_one(struct spi_master *master, struct spi_message *m) {
	struct tiny6212 *hw = spi_master_get_devdata(master);
	struct spi_device *spi = m->spi;
	struct spi_transfer *t;
	int status = 0;

	hw->tlen = 0;
	list_for_each_entry(t, &m->transfers, transfer_list) {
		bool do_transfer;

		status = tiny6212_check_transfer(spi, t, hw->tlen);
		if (status < 0) break;

		status = tiny6212_setup_transfer(hw, t->speed_hz, spi->mode);
		if (status < 0) break;

		do_transfer = (t->cs_change || list_is_last(&t->transfer_list, &m->transfers));

		if (t->len) {
			status = tiny6212_txrx(hw, m, t, do_transfer);
			if (status < 0) break;
			m->actual_length += status;
		}
		status = 0;

		spi_transfer_delay_exec(t);
	}

	m->status = status;
	spi_finalize_current_message(master);

	return status;
}

static size_t tiny6212_max_transfer_size(struct spi_device *spi) {
	return TINY6212_BUFSIZ;
}

static int tiny6212_setup(struct spi_device *spi) {
	return 0;
}

static int tiny6212_probe(struct i2c_client *client, const struct i2c_device_id *id) {
	struct device *dev = &client->dev;
	struct device_node *np = dev->of_node;
	struct tiny6212 *hw;
	struct spi_master *master;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) return -EINVAL;

	master = devm_spi_alloc_master(dev, sizeof(*hw));
	if (!master) return -ENOMEM;

	hw = spi_master_get_devdata(master);
	i2c_set_clientdata(client, hw);

	hw->master = master;
	hw->client = client;
	hw->dev = dev;
	hw->freq = TINY6212_CLOCK;
	hw->ctrl = TINY6212_MODE_DEFAULT;

	master->num_chipselect = 1;
	master->bus_num = np?-1:client->adapter->nr;
	master->mode_bits = (SPI_CPOL | SPI_CPHA | SPI_LSB_FIRST);
	master->bits_per_word_mask = SPI_BPW_MASK(8);
	master->setup = tiny6212_setup;
	master->transfer_one_message = tiny6212_transfer_one;
	master->max_transfer_size = tiny6212_max_transfer_size;
	master->max_message_size = tiny6212_max_transfer_size;
	master->dev.of_node = np;
	master->max_speed_hz = hw->freq/2;
	master->min_speed_hz = hw->freq/128;

	return devm_spi_register_master(dev, master);
}

static const struct i2c_device_id tiny6212_id[] = {
	{"tiny6212", 0},
	{}
};
MODULE_DEVICE_TABLE(i2c, tiny6212_id);

static const struct of_device_id tiny6212_of_match[] = {
	{.compatible = "pinephone,tiny6212"},
	{}
};
MODULE_DEVICE_TABLE(of, tiny6212_of_match);

static struct i2c_driver tiny6212_driver = {
	.driver = {
		.name = "tiny6212",
		.of_match_table = of_match_ptr(tiny6212_of_match),
	},
	.id_table = tiny6212_id,
	.probe = tiny6212_probe,
};

module_i2c_driver(tiny6212_driver);

MODULE_DESCRIPTION("TINY6212 SPI-I2C Bridge driver");
MODULE_AUTHOR("Egor Vorontsov <sdoregor@sdore.me>");
MODULE_LICENSE("GPL");
