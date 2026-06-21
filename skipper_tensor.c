////////////////////////////////////////////////////////////////////////////
//                            **** SKIPPER ****                           //
//                  Selective Audio Detection and Filter                  //
//                    Copyright (c) 2024 David Bryant.                    //
//                          All Rights Reserved.                          //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "lzwlib.h"
#include "skipper_tensor.h"

typedef struct {
    unsigned int size, index, wrapped;
    unsigned char *buffer;
} TensorStreamer;

static int read_buff (void *ctx);
static void write_buff (int value, void *ctx);

int read_tensor_file (tensor_array tensor, char *filename)
{
    int num_bytes = 0, alloced_bytes = 0, res, ch;
    FILE *tensor_file = fopen (filename, "rb");
    unsigned char *buffer = NULL;

    if (!tensor_file) {
        fprintf (stderr, "\nerror: can't open \"%s\" for reading!\n", filename);
        return 0;
    }

    while ((ch = getc (tensor_file)) != EOF) {
        if (num_bytes == alloced_bytes) {
            unsigned char *new_buffer = realloc (buffer, alloced_bytes += 65536);

            if (!new_buffer) {
                fclose (tensor_file);
                free (buffer);
                fprintf (stderr, "error: failed to allocate tensor input buffer!\n");
                return 0;
            }

            buffer = new_buffer;
        }

        buffer [num_bytes++] = ch;
    }

    fclose (tensor_file);
    res = local_tensor_file (tensor, buffer, num_bytes);
    free (buffer);

    return res;
}

int local_tensor_file (tensor_array tensor, unsigned char *compressed_tensor, int compressed_size)
{
    unsigned char dimensions [4] = { ARRAY_BINS_1, ARRAY_BINS_2, ARRAY_BINS_3, ARRAY_BINS_4 };
    struct tensor_header header;
    TensorStreamer reader, writer;

    if (compressed_size < (int) sizeof (header)) {
        fprintf (stderr, "invalid tensor!\n");
        return 0;
    }

    memcpy (&header, compressed_tensor, sizeof (header));
    compressed_tensor += sizeof (header);
    compressed_size -= sizeof (header);

    if (memcmp (header.dimensions, dimensions, sizeof (dimensions)) || header.version != TENSOR_VERSION) {
        fprintf (stderr, "invalid tensor!\n");
        return 0;
    }

    memset (&reader, 0, sizeof (reader));
    memset (&writer, 0, sizeof (writer));

    reader.buffer = compressed_tensor;
    reader.size = compressed_size;

    writer.buffer = (unsigned char *) tensor;
    writer.size = sizeof (tensor_array);

    if (lzw_decompress (write_buff, &writer, read_buff, &reader)) {
        fprintf (stderr, "lzw_decompress() returned error!\n");
        return 0;
    }

    if (reader.index != reader.size || writer.index != writer.size || reader.wrapped || writer.wrapped) {
        fprintf (stderr, "other error in decompressing tensor!\n");
        return 0;
    }

    for (int i = 0; i < (int) sizeof (tensor_array); ++i)
        header.checksum -= ((unsigned char *) tensor) [i];

    if (header.checksum) {
        fprintf (stderr, "checksum error in decompressed tensor!\n");
        return 0;
    }

    return 1;
}

static int read_buff (void *ctx)
{
    TensorStreamer *stream = ctx;

    if (stream->index == stream->size)
        return EOF;

    return stream->buffer [stream->index++];
}

static void write_buff (int value, void *ctx)
{
    TensorStreamer *stream = ctx;

    if (stream->index == stream->size) {
        stream->index = 0;
        stream->wrapped++;
    }

    stream->buffer [stream->index++] = value;
}
