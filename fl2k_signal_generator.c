#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <inttypes.h>
#include <unistd.h>
#include <osmo-fl2k.h>
#include <math.h>
#include <curses.h>

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
static double period_samples;

static uint8_t sine_table[10000];

static void generate_sine_table(uint8_t *buf)
{
	for (unsigned i = 0; i < sizeof(sine_table); ++i) {
		buf[i] = sinf((float)i / sizeof(sine_table) * M_PI * 2) * 0x7f + 0x80;
	}
}

static void set_target_frequency(double frequency)
{
	target_frequency = frequency;
	period_samples = (double)samp_rate / target_frequency;
}

static void fl2k_callback(fl2k_data_info_t *data_info)
{
	if (data_info->device_error) {
		fprintf(stderr, "Device error, exiting.\n");
		do_exit = 1;
		return;
	}

	data_info->sampletype_signed = 0;
	data_info->r_buf = (char *)txbuf;

	static uint64_t phase_shift = 0;
	const double phase_shift_per_sample = 1.0 / period_samples;
	double current_phase_shift = phase_shift % (uint32_t)period_samples / period_samples;	// 0.0 - 1.0
	for (unsigned i = 0; i < FL2K_BUF_LEN; ++i) {
		current_phase_shift += phase_shift_per_sample;
		if (current_phase_shift > 1) {
			current_phase_shift -= 1;
		}
		switch (waveform_setting) {
		case SAW_W:
			txbuf[i] = current_phase_shift * 0xff;
			break;
		case SINE_W:
			txbuf[i] = sine_table[(unsigned)(current_phase_shift * sizeof(sine_table))];
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
	generate_sine_table(sine_table);
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

	period_samples = (double)samp_rate / target_frequency;
	int r = fl2k_start_tx(dev, fl2k_callback, NULL, 0);

	/* Set the sample rate */
	r = fl2k_set_sample_rate(dev, samp_rate);
	if (r < 0)
		fprintf(stderr, "WARNING: Failed to set sample rate.\n");

	initscr();
	cbreak();
	keypad(stdscr, TRUE);
	noecho();

	move(1, 0);
	printw("Controls:\nLeft-Right: adjust frequency by 1%\nr: round the frequency");

	while (!do_exit) {
		int ch = getch();
		switch (ch) {
		case KEY_LEFT:
			set_target_frequency(target_frequency * 1.01);
			break;
		case KEY_RIGHT:
			set_target_frequency(target_frequency * 0.99);
			break;
		case 'r':
			;
			uint32_t tf = target_frequency;
			uint_fast8_t zeroes =
			    (tf % 10	== 0) +
			    (tf % 100	== 0) +
			    (tf % 1000	== 0) +
			    (tf % 10000	== 0) +
			    (tf % 100000	== 0) +
			    (tf % 1000000	== 0) +
			    (tf % 10000000	== 0);
			uint32_t round_to = pow(10, zeroes + 1);
			tf = tf / round_to * round_to;
			if (tf > 0) {
				set_target_frequency(tf);
			}
			break;
		}
		move(0, 0);
		clrtoeol();
		printw("Target frequency: %lf", target_frequency);
		refresh();
	}

	endwin();

	fl2k_close(dev);

out:
	if (txbuf)
		free(txbuf);


	return 0;
}
