#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <inttypes.h>
#include <unistd.h>
#include <osmo-fl2k.h>
#include <math.h>

enum waveform_e {
	SAW_W,
	SINE_W,
	SQUARE_W,
	TRIANGLE_W,
};

static fl2k_dev_t *dev = NULL;
static uint32_t samp_rate = 150000000;
static bool do_exit = false;
static uint8_t *txbuf = NULL;
static enum waveform_e waveform_setting = SINE_W;
static double target_frequency = 1000000;

void fl2k_callback(fl2k_data_info_t *data_info)
{
	if (data_info->device_error) {
		fprintf(stderr, "Device error, exiting.\n");
		do_exit = 1;
		return;
	}

	data_info->sampletype_signed = 0;
	data_info->r_buf = (char *)txbuf;

	double period_samples = (double)samp_rate / target_frequency;

	static uint64_t phase_shift = 0;
	for (unsigned i = 0; i < FL2K_BUF_LEN; ++i) {
		double current_phase_shift = (phase_shift + i) % (uint32_t)period_samples / period_samples;	// 0.0 - 1.0
		switch (waveform_setting) {
		case SAW_W:
			txbuf[i] = current_phase_shift * 0xff;
			break;
		case SINE_W:
			// TODO: pre-generate the sine waveform
			txbuf[i] = sinf(current_phase_shift * M_PI * 2) * 0x7f + 0x80;
			break;
		case SQUARE_W:
			txbuf[i] = ((phase_shift + i) % (uint32_t)period_samples) / (uint32_t)(period_samples / 2) * 0xff;
			break;
		case TRIANGLE_W:
			txbuf[i] = fabsf(1.0 - current_phase_shift * 2) * 0xff;
			break;
		}
	}
	phase_shift += FL2K_BUF_LEN;

	if (do_exit) {
		fl2k_stop_tx(dev);
	}
}

int main(int argc, char *argv[])
{
	txbuf = malloc(FL2K_BUF_LEN);
	if (!txbuf) {
		fprintf(stderr, "malloc error!\n");
		goto out;
	}

	uint32_t dev_index = 0;
	fl2k_open(&dev, dev_index);
	if (NULL == dev) {
		fprintf(stderr, "Failed to open fl2k device #%d.\n", dev_index);
		goto out;
	}

	int r = fl2k_start_tx(dev, fl2k_callback, NULL, 0);

	/* Set the sample rate */
	r = fl2k_set_sample_rate(dev, samp_rate);
	if (r < 0)
		fprintf(stderr, "WARNING: Failed to set sample rate.\n");

	while (!do_exit)
		usleep(500000);

	fl2k_close(dev);

out:
	if (txbuf)
		free(txbuf);


	return 0;
}
