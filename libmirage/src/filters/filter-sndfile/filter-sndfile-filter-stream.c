/*
 *  libMirage: SNDFILE filter: Filter stream object
 *  Copyright (C) 2012 Rok Mandeljc
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "filter-sndfile.h"

#define __debug__ "SNDFILE-FilterStream"

/* Number of frames to cache (588 frames/sector, 75 sectors/second) */
#define NUM_FRAMES 588*75


/**********************************************************************\
 *                          Private structure                         *
\**********************************************************************/
#define MIRAGE_FILTER_STREAM_SNDFILE_GET_PRIVATE(obj) (G_TYPE_INSTANCE_GET_PRIVATE((obj), MIRAGE_TYPE_FILTER_STREAM_SNDFILE, MirageFilterStreamSndfilePrivate))

struct _MirageFilterStreamSndfilePrivate
{
    SNDFILE *sndfile;
    SF_INFO format;

    gint buflen;
    guint8 *buffer;

    gint cached_block;

    /* Resampling */
    double io_ratio;
    float *resample_buffer_in;
    float *resample_buffer_out;
    SRC_STATE *resampler;
    SRC_DATA resampler_data;
};


/**********************************************************************\
 *                        libsndfile I/O bridge                        *
\**********************************************************************/
static sf_count_t sndfile_io_get_filelen (MirageStream *stream)
{
    goffset position, size;

    /* Store current position */
    position = mirage_stream_tell(stream);

    /* Seek to the end of file and get position */
    mirage_stream_seek(stream, 0, G_SEEK_END, NULL);
    size = mirage_stream_tell(stream);

    /* Restore position */
    mirage_stream_seek(stream, position, G_SEEK_SET, NULL);

    return size;
}

static sf_count_t sndfile_io_seek (sf_count_t offset, int whence, MirageStream *stream)
{
    GSeekType seek_type;

    /* Map whence parameter */
    switch (whence) {
        case SEEK_CUR: {
            seek_type = G_SEEK_CUR;
            break;
        }
        case SEEK_SET: {
            seek_type = G_SEEK_SET;
            break;
        }
        case SEEK_END: {
            seek_type = G_SEEK_END;
            break;
        }
        default: {
            /* Should not happen... */
            seek_type = G_SEEK_SET;
            break;
        }
    }

    /* Seek */
    mirage_stream_seek(stream, offset, seek_type, NULL);

    return mirage_stream_tell(stream);
}

static sf_count_t sndfile_io_read (void *ptr, sf_count_t count, MirageStream *stream)
{
    return mirage_stream_read(stream, ptr, count, NULL);
}

static sf_count_t sndfile_io_write (const void *ptr, sf_count_t count, MirageStream *stream)
{
    return mirage_stream_write(stream, ptr, count, NULL);
}

static  sf_count_t sndfile_io_tell (MirageStream *stream)
{
    return mirage_stream_tell(stream);
}

static SF_VIRTUAL_IO sndfile_io_bridge = {
    .get_filelen = (sf_vio_get_filelen)sndfile_io_get_filelen,
    .seek = (sf_vio_seek)sndfile_io_seek,
    .read = (sf_vio_read)sndfile_io_read,
    .write = (sf_vio_write)sndfile_io_write,
    .tell = (sf_vio_tell)sndfile_io_tell,
};


/**********************************************************************\
 *              MirageFilterStream methods implementations            *
\**********************************************************************/
static gboolean mirage_filter_stream_sndfile_open (MirageFilterStream *_self, MirageStream *stream, gboolean writable, GError **error)
{
    MirageFilterStreamSndfile *self = MIRAGE_FILTER_STREAM_SNDFILE(_self);
    gsize length;

    /* Clear the format */
    memset(&self->priv->format, 0, sizeof(self->priv->format));

    if (writable) {
        /* If we are creating the stream (read/write mode), we need to
           provide format data ourselves */
        const gchar *filename = mirage_stream_get_filename(stream);
        const gchar *suffix = mirage_helper_get_suffix(filename);

        self->priv->format.samplerate = 2;
        self->priv->format.channels = 44100;

        if (g_ascii_strcasecmp(suffix, ".wav")) {
            self->priv->format.format = SF_FORMAT_WAV;
        } else if (g_ascii_strcasecmp(suffix, ".aiff")) {
            self->priv->format.format = SF_FORMAT_AIFF;
        } else if (g_ascii_strcasecmp(suffix, ".flac")) {
            self->priv->format.format = SF_FORMAT_FLAC;
        } else if (g_ascii_strcasecmp(suffix, ".ogg")) {
            self->priv->format.format = SF_FORMAT_OGG;
        } else {
            MIRAGE_DEBUG(self, MIRAGE_DEBUG_WARNING, "%s: unknown file suffix '%s'; storing as raw PCM data!\n", __debug__, suffix);
            self->priv->format.format = SF_FORMAT_RAW;
        }
    }

    /* Seek to beginning */
    mirage_stream_seek(stream, 0, G_SEEK_SET, NULL);

    /* Try opening sndfile on top of stream */
    self->priv->sndfile = sf_open_virtual(&sndfile_io_bridge, SFM_READ, &self->priv->format, stream);
    if (!self->priv->sndfile) {
        g_set_error(error, MIRAGE_ERROR, MIRAGE_ERROR_CANNOT_HANDLE, "Filter cannot handle given data: failed to open sndfile!");
        return FALSE;
    }

    MIRAGE_DEBUG(self, MIRAGE_DEBUG_PARSER, "%s: audio file info:\n", __debug__);
    MIRAGE_DEBUG(self, MIRAGE_DEBUG_PARSER, "%s:  frames: %d\n", __debug__, self->priv->format.frames);
    MIRAGE_DEBUG(self, MIRAGE_DEBUG_PARSER, "%s:  samplerate: %d\n", __debug__, self->priv->format.samplerate);
    MIRAGE_DEBUG(self, MIRAGE_DEBUG_PARSER, "%s:  channels: %d\n", __debug__, self->priv->format.channels);
    MIRAGE_DEBUG(self, MIRAGE_DEBUG_PARSER, "%s:  format: %d\n", __debug__, self->priv->format.format);
    MIRAGE_DEBUG(self, MIRAGE_DEBUG_PARSER, "%s:  sections: %d\n", __debug__, self->priv->format.sections);
    MIRAGE_DEBUG(self, MIRAGE_DEBUG_PARSER, "%s:  seekable: %d\n", __debug__, self->priv->format.seekable);

    /* Check some additional requirements (two channels, seekable and samplerate) */
    if (!self->priv->format.seekable) {
        g_set_error(error, MIRAGE_ERROR, MIRAGE_ERROR_DATA_FILE_ERROR, "Audio file is not seekable!");
        return FALSE;
    }
    if (self->priv->format.channels != 2) {
        g_set_error(error, MIRAGE_ERROR, MIRAGE_ERROR_DATA_FILE_ERROR, "Invalid number of channels in audio file (%d)! Only two-channel audio files are supported!", self->priv->format.channels);
        return FALSE;
    }

    /* Compute length in bytes */
    length = self->priv->format.frames * self->priv->format.channels * sizeof(guint16);
    MIRAGE_DEBUG(self, MIRAGE_DEBUG_PARSER, "%s: raw stream length: %ld (0x%lX) bytes\n", __debug__, length, length);
    mirage_filter_stream_set_stream_length(MIRAGE_FILTER_STREAM(self), length);

    /* Allocate read buffer; we wish to hold a single (multichannel) frame */
    self->priv->buflen = self->priv->format.channels * NUM_FRAMES * sizeof(guint16);
    MIRAGE_DEBUG(self, MIRAGE_DEBUG_PARSER, "%s: buffer length: %d bytes\n", __debug__, self->priv->buflen);
    self->priv->buffer = g_try_malloc(self->priv->buflen);
    if (!self->priv->buffer) {
        g_set_error(error, MIRAGE_ERROR, MIRAGE_ERROR_PARSER_ERROR, "Failed to allocate read buffer!");
        return FALSE;
    }

    /* Initialize resampler, if needed */
    self->priv->io_ratio = self->priv->format.samplerate / 44100.0;
    if (self->priv->io_ratio != 1.0) {
        gint resampler_error;
        gint buffer_size;

        /* Initialize resampler */
        MIRAGE_DEBUG(self, MIRAGE_DEBUG_PARSER, "%s: audio stream needs to be resampled to 44.1 kHZ, initializing resampler...\n", __debug__);
        self->priv->resampler = src_new(SRC_LINEAR, self->priv->format.channels, &resampler_error);
        if (!self->priv->resampler) {
            g_set_error(error, MIRAGE_ERROR, MIRAGE_ERROR_PARSER_ERROR, "Failed to initialize resampler; error code %d!", resampler_error);
            return FALSE;
        }

        /* Allocate resampler's output buffer */
        buffer_size = self->priv->format.channels * NUM_FRAMES * sizeof(float);
        MIRAGE_DEBUG(self, MIRAGE_DEBUG_PARSER, "%s: resampler's output buffer: %d bytes\n", __debug__, buffer_size);
        self->priv->resample_buffer_out = g_try_malloc(buffer_size);
        if (!self->priv->resample_buffer_out) {
            g_set_error(error, MIRAGE_ERROR, MIRAGE_ERROR_PARSER_ERROR, "Failed to allocate resampler output buffer!");
            return FALSE;
        }

        /* Allocate resampler's input buffer */
        buffer_size *= self->priv->io_ratio;
        MIRAGE_DEBUG(self, MIRAGE_DEBUG_PARSER, "%s: resampler's input buffer: %d bytes\n", __debug__, buffer_size);
        self->priv->resample_buffer_in = g_try_malloc(buffer_size);
        if (!self->priv->resample_buffer_in) {
            g_set_error(error, MIRAGE_ERROR, MIRAGE_ERROR_PARSER_ERROR, "Failed to allocate resampler input buffer!");
            return FALSE;
        }

        /* Initialize static fields of resampler's data structure */
        self->priv->resampler_data.data_in = self->priv->resample_buffer_in;
        self->priv->resampler_data.data_out = self->priv->resample_buffer_out;
        self->priv->resampler_data.output_frames = NUM_FRAMES;
        self->priv->resampler_data.src_ratio = 1/self->priv->io_ratio;

        /* Adjust stream length */
        length = round(length/self->priv->io_ratio);
        MIRAGE_DEBUG(self, MIRAGE_DEBUG_PARSER, "%s: resampled stream length: %ld (0x%lX) bytes\n", __debug__, length, length);
        mirage_filter_stream_set_stream_length(MIRAGE_FILTER_STREAM(self), length);
    }

    return TRUE;
}

static gssize mirage_filter_stream_sndfile_partial_read (MirageFilterStream *_self, void *buffer, gsize count)
{
    MirageFilterStreamSndfile *self = MIRAGE_FILTER_STREAM_SNDFILE(_self);
    goffset position = mirage_filter_stream_get_position(_self);
    gint block;

    /* Find the block of frames corresponding to current position; this
       is within the final, possibly resampled, stream */
    block = position / self->priv->buflen;
    MIRAGE_DEBUG(self, MIRAGE_DEBUG_STREAM, "%s: stream position: %ld (0x%lX) -> block #%d (cached: #%d)\n", __debug__, position, position, block, self->priv->cached_block);

    /* If we do not have block in cache, read it */
    if (block != self->priv->cached_block) {
        gsize read_length;

        MIRAGE_DEBUG(self, MIRAGE_DEBUG_STREAM, "%s: block not cached, reading...\n", __debug__);

        if (self->priv->io_ratio == 1.0) {
            /* Seek to beginning of block */
            sf_seek(self->priv->sndfile, block*NUM_FRAMES, SEEK_SET);

            /* Read frames */
            read_length = sf_readf_short(self->priv->sndfile, (short *)self->priv->buffer, NUM_FRAMES);
            if (!read_length) {
                MIRAGE_DEBUG(self, MIRAGE_DEBUG_STREAM, "%s: block not read; EOF reached?\n", __debug__);
                return 0;
            }
        } else {
            gint resampler_error;

            /* Seek to beginning of block; this is in original,
               non-resampled, stream */
            sf_seek(self->priv->sndfile, block*NUM_FRAMES*self->priv->io_ratio, SEEK_SET);

            /* Read read frames into resampler's input buffer */
            read_length = sf_readf_float(self->priv->sndfile, self->priv->resample_buffer_in, NUM_FRAMES*self->priv->io_ratio);
            if (!read_length) {
                MIRAGE_DEBUG(self, MIRAGE_DEBUG_STREAM, "%s: block not read; EOF reached?\n", __debug__);
                return 0;
            }

            /* Set fields in data structure; most are static and have
               not changed since initialization */
            self->priv->resampler_data.input_frames = read_length;
            self->priv->resampler_data.end_of_input = 1;

            /* Reset resampler (assume blocks are unrelated) */
            src_reset(self->priv->resampler);

            /* Resample */
            resampler_error = src_process(self->priv->resampler, &self->priv->resampler_data);
            if (resampler_error) {
                MIRAGE_DEBUG(self, MIRAGE_DEBUG_WARNING, "%s: failed to resample frames: %s!\n", __debug__, src_strerror(resampler_error));
                /* Do nothing, though */
            }

            MIRAGE_DEBUG(self, MIRAGE_DEBUG_STREAM, "%s: resampler: read %ld input frames, generated %ld output frames\n", __debug__, self->priv->resampler_data.input_frames_used, self->priv->resampler_data.output_frames_gen);

            /* Convert generated frames to short */
            src_float_to_short_array(self->priv->resample_buffer_out, (short *)self->priv->buffer, NUM_FRAMES*self->priv->format.channels);
        }

        /* Store the number of currently stored block */
        self->priv->cached_block = block;
    } else {
        MIRAGE_DEBUG(self, MIRAGE_DEBUG_STREAM, "%s: block already cached\n", __debug__);
    }

    /* Copy data */
    goffset block_offset = position % self->priv->buflen;
    count = MIN(count, self->priv->buflen - block_offset);

    MIRAGE_DEBUG(self, MIRAGE_DEBUG_STREAM, "%s: offset within block: %ld, copying %d bytes\n", __debug__, block_offset, count);

    memcpy(buffer, self->priv->buffer + block_offset, count);

    return count;
}

static gssize mirage_filter_stream_sndfile_write (MirageFilterStream *_self, const void *buffer, gsize count, GError **error G_GNUC_UNUSED)
{
    MirageFilterStreamSndfile *self = MIRAGE_FILTER_STREAM_SNDFILE(_self);
    goffset position = mirage_filter_stream_get_position(_self);
    gsize write_length;

    /* Seek to position */
    sf_seek(self->priv->sndfile, position/(self->priv->format.channels * sizeof(guint16)), SEEK_SET);

    /* Write */
    write_length = sf_writef_short(self->priv->sndfile, (const short *)buffer, count/(self->priv->format.channels * sizeof(guint16)));

    /* Update stream length */
    gint length = self->priv->format.frames * self->priv->format.channels * sizeof(guint16);
    MIRAGE_DEBUG(self, MIRAGE_DEBUG_PARSER, "%s: stream length after write: %ld (0x%lX) bytes\n", __debug__, length, length);
    mirage_filter_stream_set_stream_length(MIRAGE_FILTER_STREAM(self), length);

    return write_length * self->priv->format.channels * sizeof(guint16);
}


/**********************************************************************\
 *                             Object init                            *
\**********************************************************************/
G_DEFINE_DYNAMIC_TYPE(MirageFilterStreamSndfile, mirage_filter_stream_sndfile, MIRAGE_TYPE_FILTER_STREAM);

void mirage_filter_stream_sndfile_type_register (GTypeModule *type_module)
{
    return mirage_filter_stream_sndfile_register_type(type_module);
}


static void mirage_filter_stream_sndfile_init (MirageFilterStreamSndfile *self)
{
    self->priv = MIRAGE_FILTER_STREAM_SNDFILE_GET_PRIVATE(self);

    mirage_filter_stream_generate_info(MIRAGE_FILTER_STREAM(self),
        "FILTER-SNDFILE",
        "SNDFILE File Filter",
        FALSE,
        0
    );

    self->priv->cached_block = -1;

    self->priv->sndfile = NULL;
    self->priv->buffer = NULL;

    self->priv->resample_buffer_in = NULL;
    self->priv->resample_buffer_out = NULL;
    self->priv->resampler = NULL;
}

static void mirage_filter_stream_sndfile_finalize (GObject *gobject)
{
    MirageFilterStreamSndfile *self = MIRAGE_FILTER_STREAM_SNDFILE(gobject);

    /* Close sndfile */
    if (self->priv->sndfile) {
        sf_close(self->priv->sndfile);
    }

    /* Cleanup resampler */
    if (self->priv->resampler) {
        src_delete(self->priv->resampler);
    }

    /* Free read buffer */
    g_free(self->priv->buffer);

    /* Free resampler buffers */
    g_free(self->priv->resample_buffer_in);
    g_free(self->priv->resample_buffer_out);

    /* Chain up to the parent class */
    return G_OBJECT_CLASS(mirage_filter_stream_sndfile_parent_class)->finalize(gobject);
}

static void mirage_filter_stream_sndfile_class_init (MirageFilterStreamSndfileClass *klass)
{
    GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
    MirageFilterStreamClass *filter_stream_class = MIRAGE_FILTER_STREAM_CLASS(klass);

    gobject_class->finalize = mirage_filter_stream_sndfile_finalize;

    filter_stream_class->open = mirage_filter_stream_sndfile_open;

    filter_stream_class->partial_read = mirage_filter_stream_sndfile_partial_read;
    filter_stream_class->write = mirage_filter_stream_sndfile_write;

    /* Register private structure */
    g_type_class_add_private(klass, sizeof(MirageFilterStreamSndfilePrivate));
}

static void mirage_filter_stream_sndfile_class_finalize (MirageFilterStreamSndfileClass *klass G_GNUC_UNUSED)
{
}
