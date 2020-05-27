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

#define TO_R(x) (((x) & 7) << 6)
#define TO_G(x) (((x) & 7) << 3)
#define TO_B(x) (((x) & 3) << 0)
#define TO_RGB(r, g, b) (TO_R(r) | TO_G(g) | TO_B(b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

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
bool use_rgb332 = true;

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
#define FMT(x) (use_rgb332 ? TO_RGB((x), (x), (x)) : (x))
		unsigned adjusted_index = use_rgb332 ? i ^ 4 : i;
		switch (waveform_setting) {
		case SAW_W:
			waveform_buf[adjusted_index] = FMT((uint8_t)(current_phase_shift * 0xff));
			break;
		case SINE_W:
			waveform_buf[adjusted_index] = FMT(sine_table[(unsigned)(current_phase_shift * sizeof(sine_table))]);
			break;
		case SQUARE_W:
			waveform_buf[adjusted_index] = FMT((current_phase_shift >= 0.5) * 0xff);
			break;
		case TRIANGLE_W:
			waveform_buf[adjusted_index] = FMT((uint8_t)(fabsf(1.0 - current_phase_shift * 2) * 0xff));
			break;
		}
	}
}

static void set_target_frequency(double frequency)
{
	if (frequency > 0 && frequency <= 75000000) {
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

	// we can fit thrice as many samples in rgb332 mode
	unsigned hw_buf_len = use_rgb332 ? FL2K_XFER_LEN : FL2K_BUF_LEN;

	data_info->sampletype_signed = 0;

	phase_shift %= (uint32_t)period_samples;
	if (phase_shift < waveform_buf_len - hw_buf_len) {
		// nice, our signal is fast so we can use a pre-generated waveform
		char *waveform_continued = (char *)waveform_buf + phase_shift;
		if (use_rgb332) {
			data_info->raw_buf = waveform_continued;
		} else {
			*(char **)((void *)data_info + channel) = waveform_continued;
		}
		phase_shift += hw_buf_len;
	} else {
		// generate the waveform on the fly
		const double phase_shift_per_sample = 1.0 / period_samples;
		double current_phase_shift = phase_shift % (uint32_t)period_samples / period_samples;	// 0.0 - 1.0
		for (unsigned i = 0; i < hw_buf_len; ++i) {
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
		if (use_rgb332) {
			data_info->raw_buf = (char *)txbuf;
		} else {
			*(char **)((void *)data_info + channel) = (char *)txbuf;
		}
		phase_shift += hw_buf_len;
	}

	if (do_exit) {
		fl2k_stop_tx(dev);
	}
}

int main(int argc, char *argv[])
{
	generate_sine_table(sine_table);
	txbuf = malloc(MAX(FL2K_XFER_LEN, FL2K_BUF_LEN));
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

	if (use_rgb332) {
		fl2k_set_rgb332(dev);
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

	printw("Target frequency: %lfHz", target_frequency);
	move(1, 0);
	printw("Controls:\n"
	       "Set [f]requency by typing it and hitting Enter\n"
	       "Up-Down: adjust frequency by 10%\n"
	       "Right-Left: adjust frequency by 1%\n"
	       "Setting waveform: s[q]uare, [s]ine, sa[w], [t]riangle\n"
	       "[r]ound the frequency\n"
	       "Toggle RGB[3]32 mode (higher frequency at the cost of lower resolution)\n"
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
		case '3':
			use_rgb332 = !use_rgb332;
			// adjust the sampling rate
			// TODO
			// regenerate waveform
			set_target_frequency(target_frequency);
			break;
		}
		move(0, 0);
		clrtoeol();
		printw("Target frequency: %lfHz", target_frequency);
		refresh();
	}

	endwin();

	fl2k_close(dev);

out:
	if (txbuf)
		free(txbuf);


	return 0;
}
