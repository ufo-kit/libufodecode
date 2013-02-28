#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/time.h>
#include <getopt.h>
#include <ufodecode.h>

static const int MAX_ROWS = 2048;

typedef struct {
    int clear_frame;
    int dry_run;
    int verbose;
    int rows;
    int print_frame_rate;
    int print_num_rows;
    int cont;
} Options;

static int
read_raw_file(const char *filename, char **buffer, size_t *length)
{
    FILE *fp = fopen(filename, "rb");
    if (fp == NULL)
        return ENOENT;

    fseek(fp, 0, SEEK_END);
    *length = ftell(fp);
    rewind(fp);

    *buffer = (char *) malloc(*length);

    if (*buffer == NULL) {
        fclose(fp);
        return ENOMEM;
    }

    size_t buffer_length = fread(*buffer, 1, *length, fp);
    fclose(fp);
    if (buffer_length != *length) {
        free(*buffer);
        return ERANGE;
    }
    return 0;
}

static void
usage(void)
{
    printf("usage: ipedec [OPTION]... FILE [FILE ...]\n\
Options:\n\
  -h, --help                Show this help message and exit\n\
  -v, --verbose             Print additional information on STDOUT\n\
  -r, --num-rows=N          N rows that are contained in the file\n\
  -c, --clear-frame         Clear the frame for each iteration\n\
  -d, --dry-run             Do not save the frames\n\
  -f, --print-frame-rate    Print frame rate on STDOUT\n\
      --print-num-rows      Print number of rows on STDOUT\n\
      --continue            Continue decoding frames even when errors occur\n");
}

static void
print_meta_data (UfoDecoderMeta *meta)
{
    printf("  frame_number    = %i\n", meta->frame_number);
    printf("  time_stamp      = %i\n", meta->time_stamp);
    printf("  output_mode     = %i\n", meta->output_mode);
    printf("  adc_resolution  = %i\n", meta->adc_resolution);
    printf("  n_rows          = %i\n", meta->n_rows);
    printf("  n_skipped_rows  = %i\n", meta->n_skipped_rows);

    printf("  status1\n");
    printf("    fsm_master_readout = %i\n", meta->status1.desc.fsm_master_readout);
    printf("    fsm_daq         = %i\n", meta->status1.desc.fsm_daq);
    printf("    pixel_full      = %i\n", meta->status1.desc.pixel_full);
    printf("    control_lock    = %i\n", meta->status1.desc.control_lock);
    printf("    data_lock       = %i\n", meta->status1.desc.data_lock);

    printf("  status2\n");
    printf("    end_of_frames   = %i\n", meta->status2.desc.end_of_frames);
    printf("    busy_or         = %i\n", meta->status2.desc.busy_or);
    printf("    busy_ddr        = %i\n", meta->status2.desc.busy_ddr);
    printf("    busy_interl     = %i\n", meta->status2.desc.busy_interl);
    printf("    error_status    = %i\n", meta->status2.desc.error_status);
    printf("    data_fifo_read_count = %i\n", meta->status2.desc.data_fifo_read_count);
    printf("    data_fifo_full       = %i\n", meta->status2.desc.data_fifo_full);
    printf("    data_fifo_empty      = %i\n", meta->status2.desc.data_fifo_empty);
    printf("    ddr_fifo_write_count = %i\n", meta->status2.desc.ddr_fifo_write_count);
    printf("    ddr_fifo_full        = %i\n", meta->status2.desc.ddr_fifo_full);
    printf("    ddr_fifo_empty       = %i\n", meta->status2.desc.ddr_fifo_empty);

    printf("  status3\n");
    printf("    row_counter     = %i\n", meta->status3.desc.row_counter);
    printf("    pixel_counter   = %i\n", meta->status3.desc.pixel_counter);
    printf("    ddr_read        = %i\n", meta->status3.desc.ddr_read);
    printf("    ddr_write       = %i\n", meta->status3.desc.ddr_write);
    printf("    ddr_arbiter     = %i\n", meta->status3.desc.ddr_arbiter);
    printf("\n");
}

typedef struct {
    struct timeval  start;
    long            seconds;
    long            useconds;
} Timer;

static Timer *
timer_new (void)
{
    Timer *t = (Timer *) malloc (sizeof (Timer));
    t->seconds = t->useconds = 0L;
    return t;
}

static void
timer_destroy (Timer *t)
{
    free (t);
}

static void
timer_start (Timer *t)
{
    gettimeofday(&t->start, NULL);
}

static void
timer_stop (Timer *t)
{
    struct timeval end;

    gettimeofday(&end, NULL);
    t->seconds += end.tv_sec - t->start.tv_sec;
    t->useconds += end.tv_usec - t->start.tv_usec;
}

static int
process_file(const char *filename, Options *opts)
{
    UfoDecoder      *decoder;
    UfoDecoderMeta   meta = {0};
    Timer           *timer;
    char            *buffer;
    size_t           num_bytes;
    uint16_t        *pixels;
    uint32_t         time_stamp, old_time_stamp;
    int              n_frames;
    int              error = 0;
    FILE            *fp;
    char             output_name[256];
    float            mtime;

    error = read_raw_file(filename, &buffer, &num_bytes);

    if (error) {
        fprintf(stderr, "Error reading %s: %s\n", filename, strerror(error));
        return error;
    }

    decoder = ufo_decoder_new(opts->rows, 2048, (uint32_t *) buffer, num_bytes);

    if (decoder == NULL) {
        fprintf(stderr, "Failed to initialize decoder\n");
        return 1;
    }

    if (!opts->dry_run) {
        snprintf(output_name, 256, "%s.raw", filename);
        fp = fopen(output_name, "wb");

        if (!fp) {
            fprintf(stderr, "Failed to open file for writing\n");
            return 1;
        }
    }

    timer = timer_new ();
    pixels = (uint16_t *) malloc(2048 * MAX_ROWS * sizeof(uint16_t));
    n_frames = 0;
    old_time_stamp = 0;

    while (error != EIO) {
        if (opts->clear_frame)
            memset(pixels, 0, 2048 * opts->rows * sizeof(uint16_t));

        timer_start (timer);
        error = ufo_decoder_get_next_frame(decoder, &pixels, &meta);
        timer_stop (timer);
        n_frames++;

        if (!error) {

            if (opts->verbose) {
                printf("Status for frame %i\n", n_frames);
                print_meta_data (&meta);
            }

            if (opts->print_frame_rate) {
                uint32_t diff = 80 * (meta.time_stamp - old_time_stamp);

                printf("%-6d", 1000000000 / diff);
                old_time_stamp = meta.time_stamp;
            }

            if (opts->print_num_rows)
                printf("%d", meta.n_rows); 

            if (opts->print_frame_rate || opts->print_num_rows)
                printf("\n");

            if (!opts->dry_run)
                fwrite(pixels, sizeof(uint16_t), 2048 * opts->rows , fp);
        }
        else if (error != EIO) {
            fprintf(stderr, "Failed to decode frame %i\n", n_frames);

            if (opts->cont) {
                /* Save the frame even though we know it is corrupted */
                if (!opts->dry_run)
                    fwrite(pixels, sizeof(uint16_t), 2048 * opts->rows, fp);
            }
            else
                break;
        }
    }

    if (!opts->dry_run)
        fclose(fp);

    if (opts->verbose) {
        mtime = timer->seconds * 1000.0 + timer->useconds / 1000.0;
        printf("Decoded %i frames in %.5fms\n", n_frames, mtime);
    }

    free(pixels);
    free(buffer);
    timer_destroy (timer);
    ufo_decoder_free(decoder);

    return error == EIO ? 0 : error;
}

int main(int argc, char const* argv[])
{
    int getopt_ret, index;

    enum {
        CLEAR_FRAME  = 'c',
        DRY_RUN      = 'd',
        FRAME_RATE   = 'f',
        HELP         = 'h',
        SET_NUM_ROWS = 'r', 
        VERBOSE      = 'v',
        CONTINUE,
        NUM_ROWS
    };

    static struct option long_options[] = {
        { "num-rows",           required_argument, 0, SET_NUM_ROWS },
        { "clear-frame",        no_argument, 0, CLEAR_FRAME },
        { "verbose",            no_argument, 0, VERBOSE },
        { "help",               no_argument, 0, HELP },
        { "dry-run",            no_argument, 0, DRY_RUN },
        { "print-frame-rate",   no_argument, 0, FRAME_RATE },
        { "continue",           no_argument, 0, CONTINUE },
        { "print-num-rows",     no_argument, 0, NUM_ROWS },
        { 0, 0, 0, 0 }
    };

    static Options opts = {
        .rows = 1088,
        .verbose = 0,
        .dry_run = 0,
        .clear_frame = 0,
        .print_frame_rate = 0,
        .print_num_rows = 0,
        .cont = 0
    };

    while ((getopt_ret = getopt_long(argc, (char *const *) argv, "r:cvhdf", long_options, &index)) != -1) {
        switch (getopt_ret) {
            case SET_NUM_ROWS:
                opts.rows = atoi(optarg);
                break;
            case CLEAR_FRAME:
                opts.clear_frame = 1;
                break;
            case VERBOSE:
                opts.verbose = 1;
                break;
            case HELP:
                usage();
                return 0;
            case DRY_RUN:
                opts.dry_run = 1;
                break;
            case FRAME_RATE:
                opts.print_frame_rate = 1;
                break;
            case CONTINUE:
                opts.cont = 1;
                break;
            case NUM_ROWS:
                opts.print_num_rows = 1;
            default:
                break;
        }
    }

    if (optind == argc) {
        printf("ipedec: no input files\n");
        return 1;
    }

    while (optind < argc) {
        int errcode = process_file(argv[optind++], &opts);

        if (errcode != 0)
            return errcode;
    }

    return 0;
}

