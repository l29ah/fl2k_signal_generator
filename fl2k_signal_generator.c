#include <stddef.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <inttypes.h>
#include <unistd.h>
#include <osmo-fl2k.h>
#include <math.h>
#include <error.h>
#include <curses.h>
#include <locale.h>

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
static uint8_t *waveform_buf = NULL;
static const size_t waveform_buf_len = FL2K_BUF_LEN * 10;
static enum waveform_e waveform_setting = SINE_W;
static double target_frequency = 1000000;
static double period_samples;
static unsigned channel = offsetof(fl2k_data_info_t, r_buf);

static uint8_t sine_table[10000];

static void generate_sine_table(uint8_t *buf)
{
	for (unsigned i = 0; i < sizeof(sine_table); ++i) {
		buf[i] = sinf((float)i / sizeof(sine_table) * M_PI * 2) * 0x7f + 0x80;
	}
}

static void regenerate_waveform()
{
	const double phase_shift_per_sample = 1.0 / period_samples;
	double current_phase_shift = 0;
	for (unsigned i = 0; i < waveform_buf_len; ++i) {
		current_phase_shift += phase_shift_per_sample;
		if (current_phase_shift > 1) {
			current_phase_shift -= 1;
		}
		if (current_phase_shift > 1) {
			endwin();
			error(-1, 0, "Signal frequency (%lfHz) is too large for the current sample rate (%uSPS)!", target_frequency, samp_rate);
		}
		switch (waveform_setting) {
		case SAW_W:
			waveform_buf[i] = current_phase_shift * 0xff;
			break;
		case SINE_W:
			waveform_buf[i] = sine_table[(unsigned)(current_phase_shift * sizeof(sine_table))];
			break;
		case SQUARE_W:
			waveform_buf[i] = (current_phase_shift >= 0.5) * 0xff;
			break;
		case TRIANGLE_W:
			waveform_buf[i] = fabsf(1.0 - current_phase_shift * 2) * 0xff;
			break;
		}
	}
}

static void set_target_frequency(double frequency)
{
	if (frequency > 0 && frequency <= (samp_rate / 2)) {
		target_frequency = frequency;
		period_samples = (double)samp_rate / target_frequency;
		regenerate_waveform();
	}
}

static void set_waveform(enum waveform_e waveform)
{
	waveform_setting = waveform;
	regenerate_waveform();
}

static void fl2k_callback(fl2k_data_info_t *data_info)
{
	if (data_info->device_error) {
		fprintf(stderr, "Device error, exiting.\n");
		do_exit = 1;
		return;
	}

	static uint64_t phase_shift = 0;
	data_info->sampletype_signed = 0;

	phase_shift %= (uint32_t)period_samples;
	if (phase_shift < waveform_buf_len - FL2K_BUF_LEN) {
		// nice, our signal is fast so we can use a pre-generated waveform
		*(char **)((void *)data_info + channel) = (char *)waveform_buf + phase_shift;
		phase_shift += FL2K_BUF_LEN;
	} else {
		// generate the waveform on the fly
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
				txbuf[i] = (current_phase_shift >= 0.5) * 0xff;
				break;
			case TRIANGLE_W:
				txbuf[i] = fabsf(1.0 - current_phase_shift * 2) * 0xff;
				break;
			}
		}
		*(char **)((void *)data_info + channel) = (char *)txbuf;
		phase_shift += FL2K_BUF_LEN;
	}

	if (do_exit) {
		fl2k_stop_tx(dev);
	}
}

int main(int argc, char *argv[])
{
	setlocale(LC_NUMERIC, "en_US");	// force some grouping characters
	generate_sine_table(sine_table);
	txbuf = malloc(FL2K_BUF_LEN);
	if (!txbuf) {
		fprintf(stderr, "malloc error!\n");
		goto out;
	}
	waveform_buf = malloc(waveform_buf_len);
	if (!waveform_buf) {
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
	if (r < 0) {
		fprintf(stderr, "Couldn't start the transmission.\n");
	}

	/* Set the sample rate */
	r = fl2k_set_sample_rate(dev, samp_rate);
	if (r < 0)
		fprintf(stderr, "WARNING: Failed to set sample rate.\n");

	// initialize the waveform buffer
	set_waveform(SINE_W);

	initscr();
	cbreak();
	keypad(stdscr, TRUE);
	noecho();

	printw("Target frequency: %'lfHz", target_frequency);
	move(1, 0);
	printw("Controls:\n"
	       "Set [f]requency by typing it and hitting Enter\n"
	       "Up-Down: adjust frequency by 10%%\n"
	       "Right-Left: adjust frequency by 1%%\n"
	       "Setting waveform: s[q]uare, [s]ine, sa[w], [t]riangle\n"
	       "[r]ound the frequency\n"
	       "Choose the channel: [R]ed, [G]reen, [B]lue. Warning: inactive channel won't be updated.\n"
	      );

	while (!do_exit) {
		int ch = getch();
		switch (ch) {
		case 'f':
			move(0, 0);
			clrtoeol();
			printw("Enter the desired frequency, Hz: ");
			echo();
			double frequency;
			scanw("%lf", &frequency);
			set_target_frequency(frequency);
			noecho();
			break;
		case KEY_RIGHT:
			set_target_frequency(target_frequency * 1.01);
			break;
		case KEY_LEFT:
			set_target_frequency(target_frequency / 1.01);
			break;
		case KEY_UP:
			set_target_frequency(target_frequency * 1.1);
			break;
		case KEY_DOWN:
			set_target_frequency(target_frequency / 1.1);
			break;
		case 'q':
			set_waveform(SQUARE_W);
			break;
		case 's':
			set_waveform(SINE_W);
			break;
		case 'w':
			set_waveform(SAW_W);
			break;
		case 't':
			set_waveform(TRIANGLE_W);
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
		case 'R':
			channel = offsetof(fl2k_data_info_t, r_buf);
			break;
		case 'G':
			channel = offsetof(fl2k_data_info_t, g_buf);
			break;
		case 'B':
			channel = offsetof(fl2k_data_info_t, b_buf);
			break;
		}
		move(0, 0);
		clrtoeol();
		printw("Target frequency: %'lfHz", target_frequency);
		refresh();
	}

	endwin();

	fl2k_close(dev);

out:
	if (txbuf)
		free(txbuf);


	return 0;
}
