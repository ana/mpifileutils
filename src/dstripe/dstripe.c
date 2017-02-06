/*
 * Copyright (c) 2013-2015, Lawrence Livermore National Security, LLC.
 *   Produced at the Lawrence Livermore National Laboratory
 *   CODE-673838
 *
 * Copyright (c) 2006-2007,2011-2015, Los Alamos National Security, LLC.
 *   (LA-CC-06-077, LA-CC-10-066, LA-CC-14-046)
 *
 * Copyright (2013-2015) UT-Battelle, LLC under Contract No.
 *   DE-AC05-00OR22725 with the Department of Energy.
 *
 * Copyright (c) 2015, DataDirect Networks, Inc.
 * 
 * All rights reserved.
 *
 * This file is part of mpiFileUtils.
 * For details, see https://github.com/hpc/fileutils.
 * Please also read the LICENSE file.
*/

// mpicc -g -O0 -o restripe restripe.c -llustreapi

#include "config.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <limits.h>
#include <getopt.h>
#include "mpi.h"

#ifdef LUSTRE_SUPPORT
#include <lustre/lustreapi.h>
#endif

#include "mfu.h"

static void print_usage(void)
{
    printf("\n");
    printf("Usage: dstripe [options] <stripes> <stripesize> <input> <output>...\n");
    printf("\n");
    printf("Options:\n");
    printf("  -r, --report           - input file stripe info\n");
    printf("  -h, --help             - print usage\n");
    printf("\n");
    fflush(stdout);
    return;
}

int main(int argc, char* argv[])
{
    MPI_Init(&argc, &argv);
    mfu_init();

    /* get our rank and number of ranks in the job */
    int rank, ranks;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &ranks);

    int option_index = 0;
    int usage = 0;
    int report = 0;
    int stripes;
    unsigned long long stripe_size;
    char *in_path;
    char *out_path;

    static struct option long_options[] = {
        {"help",     0, 0, 'h'},
        {"report",   0, 0, 'r'},
        {0, 0, 0, 0}
    };

    while (1) {
        int c = getopt_long(
                    argc, argv, "rh",
                    long_options, &option_index
                );

        if (c == -1) {
            break;
        }

        switch (c) {
            case 'r':
		report = 1;
                break;
            case 'h':
                usage = 1;
                break;
            case '?':
                usage = 1;
                break;
            default:
                if (rank == 0) {
                    printf("?? getopt returned character code 0%o ??\n", c);
                }
        }
    }

    /* check that we got the right number of parameters */
    if ((argc - optind) == 4) {
        /* get number of stripes from command line */
        stripes = atoi(argv[optind++]);

        /* parse stripe size string */
        if (mfu_abtoull(argv[optind], &stripe_size) != MFU_SUCCESS) {
            if (rank == 0) {
                printf("Failed to parse stripe size: %s\n", argv[optind]);
                fflush(stdout);
            }
            MPI_Abort(MPI_COMM_WORLD, 1);
        }
        optind++;

        /* get source and destination paths */
        in_path = argv[optind++];
        out_path = argv[optind++];
    } else {
        usage = 1;
    }

    if (usage) {
        if (rank == 0) {
            print_usage();
        }

        mfu_finalize();
        MPI_Finalize();
        return 1;
    }

    /* nothing to do if lustre support is disabled */
#ifndef LUSTRE_SUPPORT
    if (rank == 0) {
        printf("Lustre support is disabled\n");
        fflush(stdout);
    }
    MPI_Abort(MPI_COMM_WORLD, 1);
#endif

    /* TODO: verify that source / target are on Lustre */

#ifdef LUSTRE_SUPPORT
    if (report) {
        /* just have rank 0 report striping info */
        if (rank == 0) {
            struct lov_user_md lum;
            int rc = llapi_file_get_stripe(in_path, &lum);
            if (rc != 0) {
                printf("retrieving file stripe information has failed, %s\n", strerror(-rc));
                fflush(stdout);
                MPI_Abort(MPI_COMM_WORLD, 1);
            }

            printf("File \"%s\" has a stripe count of %d and a stripe size of %d bytes.\n",
                       in_path, lum.lmm_stripe_count, lum.lmm_stripe_size);
        }

        mfu_finalize();
        MPI_Finalize();
        return 0;
    }
#endif

    /* lustre requires stripe sizes to be aligned */
    if (stripe_size % 65536 != 0) {
        if (rank == 0) {
            printf("Stripe size must be a multiple of 65536\n");
            fflush(stdout);
        }
        MPI_Abort(MPI_COMM_WORLD, 1);
    }

    /* set striping params on new file */
#ifdef LUSTRE_SUPPORT
    /* just have rank 0 create the file */
    if (rank == 0) {
        int rc = llapi_file_create(out_path, stripe_size, 0, stripes, LOV_PATTERN_RAID0);
        if (rc < 0) {
            printf("file creation has failed, %s\n", strerror(-rc));
            fflush(stdout);
            MPI_Abort(MPI_COMM_WORLD, 1);
        }
    }
#endif

    MPI_Barrier(MPI_COMM_WORLD);

    /* have rank 0 read the mode and size */
    int mode;
    uint64_t file_size;
    if (rank == 0) {
        struct stat file_stat;
        if (mfu_lstat(in_path, &file_stat) < 0) {
            printf("Failed to stat file %s (%s)", in_path, strerror(errno));
            fflush(stdout);
            MPI_Abort(MPI_COMM_WORLD, 1);
        }

        /* get file mode and size from stat info */
        mode = (int) file_stat.st_mode;
        file_size = (uint64_t) file_stat.st_size;
    }
    MPI_Bcast(&mode, 1, MPI_INT, 0, MPI_COMM_WORLD);
    MPI_Bcast(&file_size, 1, MPI_UINT64_T, 0, MPI_COMM_WORLD);

    /* allocate buffer */
    size_t chunk_size = 1024*1024;
    void* buf = MFU_MALLOC(chunk_size);
    if (buf == NULL) {
        printf("Failed to allocate buffer\n");
        fflush(stdout);
        MPI_Abort(MPI_COMM_WORLD, 1);
    }

    /* open input file for reading */
    int in_fd = mfu_open(in_path, O_RDONLY);
    if (in_fd < 0) {
        printf("Failed to open input file %s (%s)\n", in_path, strerror(errno));
        fflush(stdout);
        MPI_Abort(MPI_COMM_WORLD, 1);
    }

    /* open output file for writing */
    int out_fd = mfu_open(out_path, O_WRONLY);
    if (out_fd < 0) {
        printf("Failed to open output file %s (%s)\n", out_path, strerror(errno));
        fflush(stdout);
        MPI_Abort(MPI_COMM_WORLD, 1);
    }

    /* write data */
    uint64_t base = 0;
    int done = 0;
    while (! done) {
        uint64_t chunk_id = 0;
        uint64_t stripe_read = 0;
        while (stripe_read < stripe_size) {
            /* determine number of bytes to read */
            /* try to read a full chunk's worth of bytes */
            size_t read_size = chunk_size;

            /* if the stripe doesn't have that much left */
            uint64_t remainder = stripe_size - stripe_read;
            if (remainder < (uint64_t) read_size) {
                read_size = (size_t) remainder;
            }

            /* get byte offset to read from */
            uint64_t offset = base + rank * stripe_size + chunk_id * chunk_size;
            if (offset < file_size) {
                /* the first byte falls within the file size,
                 * now check the last byte */
                uint64_t last = offset + (uint64_t) read_size;
                if (last > file_size) {
                    /* the last byte is beyond the end, set read size
                     * to the most we can read */
                    read_size = (size_t) (file_size - offset);
                }
            } else {
                /* the first byte we need to read is past the end of
                 * the file, so don't read anything */
                read_size = 0;
                done = 1;
            }

            /* bail if we don't have anything to read */
            if (read_size == 0) {
                break;
            }

            /* seek to correct spot in input file */
            off_t pos = (off_t) offset;
            off_t seek_rc = mfu_lseek(in_path, in_fd, pos, SEEK_SET);
            if (seek_rc == (off_t)-1) {
                printf("Failed to seek in input file %s (%s)\n", in_path, strerror(errno));
                fflush(stdout);
                MPI_Abort(MPI_COMM_WORLD, 1);
            }

            /* read chunk from input */
            ssize_t nread = mfu_read(in_path, in_fd, buf, read_size);

            /* check for errors */
            if (nread < 0) {
                printf("Failed to read data from input file %s (%s)\n", in_path, strerror(errno));
                fflush(stdout);
                MPI_Abort(MPI_COMM_WORLD, 1);
            }

            /* check for short reads */
            if (nread != read_size) {
                printf("Got a short read from input file %s\n", in_path);
                fflush(stdout);
                MPI_Abort(MPI_COMM_WORLD, 1);
            }

            /* seek to correct spot in output file */
            seek_rc = mfu_lseek(out_path, out_fd, pos, SEEK_SET);
            if (seek_rc == (off_t)-1) {
                printf("Failed to seek in output file %s (%s)\n", out_path, strerror(errno));
                fflush(stdout);
                MPI_Abort(MPI_COMM_WORLD, 1);
            }

            /* write chunk to output */
            ssize_t nwrite = mfu_write(out_path, out_fd, buf, read_size);

            /* check for errors */
            if (nwrite < 0) {
                printf("Failed to write data to output file %s (%s)\n", out_path, strerror(errno));
                fflush(stdout);
                MPI_Abort(MPI_COMM_WORLD, 1);
            }

            /* check for short reads */
            if (nwrite != read_size) {
                printf("Got a short write to output file %s\n", out_path);
                fflush(stdout);
                MPI_Abort(MPI_COMM_WORLD, 1);
            }

            /* go on to the next chunk in this stripe, we assume we
             * read the whole chunk size, if we didn't it's because
             * the stripe is smaller or we're at the end of the file,
             * but in either case we're done so it doesn't hurt to
             * over estimate in this calculation */
            stripe_read += (uint64_t) chunk_size;
            chunk_id++;
        }

        /* go on to next stripe */
        base += ranks * stripe_size;
    }

    /* close files */
    mfu_fsync(out_path, out_fd);
    mfu_close(out_path, out_fd);
    mfu_close(in_path, in_fd);

    /* wait for everyone to finish */
    MPI_Barrier(MPI_COMM_WORLD);

    /* set file mode */
    if (rank == 0) {
        if (truncate(out_path, (off_t) file_size) != 0) {
            printf("Failed to truncate file %s (%s)", out_path, strerror(errno));
            fflush(stdout);
            MPI_Abort(MPI_COMM_WORLD, 1);
        }

        if (mfu_chmod(out_path, (mode_t) mode) != 0) {
            printf("Failed to chmod file %s (%s)", out_path, strerror(errno));
            fflush(stdout);
            MPI_Abort(MPI_COMM_WORLD, 1);
        }
    }

    /* free buffer */
    mfu_free(&buf);

    /* wait for everyone to finish */
    MPI_Barrier(MPI_COMM_WORLD);

    mfu_finalize();
    MPI_Finalize();

    return 0;
}
