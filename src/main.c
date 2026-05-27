#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include "aula.h"

/*
 * Long options table for getopt_long.
 * each entry is: { name, has_arg, flag, val }
 *
 *   name    - the long option string (after --)
 *   has_arg - no_argument, required_argument, or optional_argument
 *   flag    - always NULL for us (used for setting variables directly)
 *   val     - the short option character returned when this flag is seen
 */
static struct option long_opts[] = {
	{ "gif",    required_argument, NULL, 'g' },
	{ "time",   no_argument,       NULL, 't' },
	{ "year",   required_argument, NULL, 'Y' },
	{ "month",  required_argument, NULL, 'M' },
	{ "day",    required_argument, NULL, 'D' },
	{ "hour",   required_argument, NULL, 'h' },
	{ "minute", required_argument, NULL, 'm' },
	{ "second", required_argument, NULL, 's' },
	{ "reset",  no_argument,       NULL, 'R' },
	{ "help",   no_argument,	   NULL, '?' },
	{ NULL,     0,                 NULL,  0  } /* sentinel: marks end of table */
};

static void print_usage(const char *prog) {
	printf("Usage: %s [command] [options]\n\n", prog);
	printf("Commands:\n");
	printf("  --gif, -g <file>   Upload a 128x128 GIF to the display\n");
	printf("					 (also syncs time automatically)\n");
	printf("  --time, -t		 Sync system time to keyboard\n\n");
	printf("Time override options (used with --time):\n");
	printf("  --year,   -Y <n>   Override year\n");
	printf("  --month,  -M <n>   Override month (1-12)\n");
	printf("  --day,    -D <n>   Override day (1-31)\n");
	printf("  --hour,   -h <n>   Override hour (0-23)\n");
	printf("  --minute, -m <n>   Override minute (0-59)\n");
	printf("  --second, -s <n>   Override second (0-59)\n\n");
	printf("Examples:\n");
	printf("  %s --gif animation.gif\n", prog);
	printf("  %s --time\n", prog);
	printf("  %s --time --hour 12 --minute 0\n", prog);
}

int main(int argc, char *argv[]) {
	/* -- Argument state -- */
	char *gif_path = NULL;
	int do_time    = 0;
	int do_gif	   = 0;
	int do_reset   = 0;

	/*
	 * Time overrides. -1 means "not set by user, use system value."
	 * atoi() converts a string like "23" to the integer 23.
	 */
	int ov_year = -1, ov_mon = -1, ov_mday = -1;
	int ov_hour = -1, ov_min = -1, ov_sec  = -1;

	/* --- Parse arguments --- */
	int opt;
	int opt_idx = 0;

	/*
	 * getopt_long processes argv one flag at a time.
	 * The short option string "g:tY:M:D:H:M:S:?"
	 * lists valid short flags. A colon after a letter
	 * means that flag requires an argument.
	 * optarg is a global set by getopt_long to point
	 * at the argument string for the current flag.
	 */
	while ((opt = getopt_long(argc, argv, "g:tY:M:D:h:m:s:R:?",
					long_opts, &opt_idx)) != -1) {
		switch(opt) {
			case 'g':
				gif_path = optarg;
				do_gif   = 1;
				// do_time  = 1; /* always sync time with gif */
				break;
			case 't':
				do_time = 1;
				break;
			case 'Y': ov_year = atoi(optarg); break;
			case 'M': ov_mon  = atoi(optarg); break;
			case 'D': ov_mday = atoi(optarg); break;
			case 'h': ov_hour = atoi(optarg); break;
			case 'm': ov_min  = atoi(optarg); break;
			case 's': ov_sec  = atoi(optarg); break;
			case 'R': do_reset = 1;           break;
			case '?':
					  print_usage(argv[0]);
					  return EXIT_SUCCESS;
			default:
					  print_usage(argv[0]);
					  return EXIT_FAILURE;
		}
	}

	/* Require at least one command */
	if (!do_gif && !do_time && !do_reset) {
		print_usage(argv[0]);
		return EXIT_FAILURE;
	}

	/* --- Open device --- */
	aula_device_t dev;
	int ret = aula_open(&dev);
	if (ret != AULA_OK)
		return EXIT_FAILURE;

	/* --- Execute commands --- */

	/*
	 * Time sync runs first so the display shows correct
	 * time from the moment the GIF starts playing.
	 */
	if (do_time) {
		ret = aula_sync_time(&dev,
				ov_year, ov_mon, ov_mday, ov_hour, ov_min, ov_sec);
		if (ret != AULA_OK) {
			aula_close(&dev);
			return EXIT_FAILURE;
		}
	}

	if (do_gif) {
		ret = aula_send_gif(&dev, gif_path);
		if (ret != AULA_OK) {
			aula_close(&dev);
			return EXIT_FAILURE;
		}
	}

	if (do_reset) {
		ret = aula_factory_reset(&dev);
		if (ret != AULA_OK) {
			aula_close(&dev);
			return EXIT_FAILURE;
		}
	}

	/* --- Clean up --- */
	aula_close(&dev);
	return EXIT_SUCCESS;
}
