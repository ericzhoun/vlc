/*****************************************************************************
 * chaptersplit.c: split the stream output at chapter boundaries
 *****************************************************************************
 * Copyright (C) 2026 VLC authors and VideoLAN
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_arrays.h>
#include <vlc_configuration.h>
#include <vlc_plugin.h>
#include <vlc_block.h>
#include <vlc_sout.h>

/*****************************************************************************
 * Exported prototypes
 *****************************************************************************/
static int      Open    ( vlc_object_t * );

/*****************************************************************************
 * Module descriptor
 *****************************************************************************/
#define DST_TEXT N_("Destination file")
#define DST_LONGTEXT N_( \
    "Destination file of the first part. Each chapter is written to a " \
    "separate file whose name is derived from it by inserting the part " \
    "number before the extension (or by replacing a %d placeholder)." )

#define MUX_TEXT N_("Muxer")
#define MUX_LONGTEXT N_( \
    "Muxer used for every generated file, e.g. mp4." )

#define EXT_TEXT N_("Extension")
#define EXT_LONGTEXT N_( \
    "Extension appended to the generated file names when the destination " \
    "does not provide one. Defaults to the muxer name." )

#define SOUT_CFG_PREFIX "sout-chaptersplit-"

vlc_module_begin ()
    set_description( N_("Chapter splitting stream output") )
    set_capability( "sout output", 0 )
    add_shortcut( "chaptersplit" )
    set_shortname( N_("Chaptersplit") )

    set_subcategory( SUBCAT_SOUT_STREAM )

    add_string( SOUT_CFG_PREFIX "dst", "", DST_TEXT, DST_LONGTEXT )
    add_string( SOUT_CFG_PREFIX "mux", "mp4", MUX_TEXT, MUX_LONGTEXT )
    add_string( SOUT_CFG_PREFIX "ext", "", EXT_TEXT, EXT_LONGTEXT )

    set_callback( Open )
vlc_module_end ()

/* */
static const char *const ppsz_sout_options[] = {
    "dst", "mux", "ext",
    NULL
};

/* */
static void *Add( sout_stream_t *, const es_format_t *, const char * );
static void  Del( sout_stream_t *, void * );
static int   Send( sout_stream_t *, void *, block_t * );
static int   Control( sout_stream_t *, int, va_list );
static void  SetPCR( sout_stream_t *, vlc_tick_t );
static void  Close( sout_stream_t * );

typedef struct sout_stream_id_sys_t sout_stream_id_sys_t;
struct sout_stream_id_sys_t
{
    es_format_t fmt;
    const char *es_id;

    block_t *p_first;
    block_t **pp_last;

    void *id;

    bool b_wait_key;
};

typedef struct
{
    char *psz_dst;
    char *psz_mux;
    char *psz_ext;

    sout_stream_t *p_out;

    bool b_split_pending;
    bool b_chapter_valid;
    bool b_failed;

    int              i_id;
    sout_stream_id_sys_t **id;
    int              i_video_id;

    unsigned i_part;
    int i_title;
    int i_seekpoint;
} sout_stream_sys_t;

static void OutputStart( sout_stream_t *p_stream );
static void OutputSend( sout_stream_t *p_stream, sout_stream_id_sys_t *id, block_t * );
static void SplitOutput( sout_stream_t *p_stream );

static const struct sout_stream_operations ops = {
    .add = Add,
    .del = Del,
    .send = Send,
    .control = Control,
    .set_pcr = SetPCR,
    .close = Close,
};

/*****************************************************************************
 * Open:
 *****************************************************************************/
static int Open( vlc_object_t *p_this )
{
    sout_stream_t *p_stream = (sout_stream_t*)p_this;
    sout_stream_sys_t *p_sys;

    p_stream->p_sys = p_sys = malloc( sizeof(*p_sys) );
    if( !p_sys )
        return VLC_ENOMEM;

    config_ChainParse( p_stream, SOUT_CFG_PREFIX, ppsz_sout_options, p_stream->p_cfg );

    p_sys->psz_dst = var_GetNonEmptyString( p_stream, SOUT_CFG_PREFIX "dst" );
    if( !p_sys->psz_dst )
    {
        msg_Err( p_stream, "no destination specified" );
        free( p_sys );
        return VLC_EGENERIC;
    }

    p_sys->psz_mux = var_GetNonEmptyString( p_stream, SOUT_CFG_PREFIX "mux" );
    if( !p_sys->psz_mux )
        p_sys->psz_mux = strdup( "mp4" );

    p_sys->psz_ext = var_GetNonEmptyString( p_stream, SOUT_CFG_PREFIX "ext" );
    if( !p_sys->psz_ext )
    {
        /* Default to the muxer name, stripped from any options */
        char *psz_brace = strchr( p_sys->psz_mux, '{' );
        size_t i_len = psz_brace ? (size_t)(psz_brace - p_sys->psz_mux)
                                 : strlen( p_sys->psz_mux );
        p_sys->psz_ext = strndup( p_sys->psz_mux, i_len );
    }

    p_sys->p_out = NULL;
    p_sys->b_split_pending = false;
    p_sys->b_chapter_valid = false;
    p_sys->b_failed = false;
    p_sys->i_id = 0;
    p_sys->id = NULL;
    p_sys->i_video_id = 0;
    p_sys->i_part = 0;
    p_sys->i_title = -1;
    p_sys->i_seekpoint = -1;
    TAB_INIT( p_sys->i_id, p_sys->id );
    p_stream->ops = &ops;

    return VLC_SUCCESS;
}

/*****************************************************************************
 * Close:
 *****************************************************************************/
static void Close( sout_stream_t *p_stream )
{
    sout_stream_sys_t *p_sys = p_stream->p_sys;

    /* If a chapter split is pending, flush what was buffered into a last
     * part. The video of that part only starts at its next keyframe. */
    if( p_sys->b_split_pending )
    {
        bool b_buffered = false;
        for( int i = 0; i < p_sys->i_id && !b_buffered; i++ )
            b_buffered = p_sys->id[i]->p_first != NULL;

        if( b_buffered )
        {
            p_sys->b_split_pending = false;
            SplitOutput( p_stream );
        }
    }

    if( p_sys->p_out )
        sout_StreamChainDelete( p_sys->p_out, NULL );

    for( int i = 0; i < p_sys->i_id; i++ )
    {
        sout_stream_id_sys_t *id = p_sys->id[i];

        if( id->p_first )
            block_ChainRelease( id->p_first );
        es_format_Clean( &id->fmt );
        free( id );
    }
    TAB_CLEAN( p_sys->i_id, p_sys->id );

    free( p_sys->psz_dst );
    free( p_sys->psz_mux );
    free( p_sys->psz_ext );
    free( p_sys );
}

/*****************************************************************************
 *
 *****************************************************************************/
static void *
Add( sout_stream_t *p_stream, const es_format_t *p_fmt, const char *es_id )
{
    sout_stream_sys_t *p_sys = p_stream->p_sys;
    sout_stream_id_sys_t *id;

    id = malloc( sizeof(*id) );
    if( !id )
        return NULL;

    es_format_Copy( &id->fmt, p_fmt );
    id->es_id = es_id;
    id->p_first = NULL;
    id->pp_last = &id->p_first;
    id->id = NULL;
    id->b_wait_key = true;

    TAB_APPEND( p_sys->i_id, p_sys->id, id );
    if( p_fmt->i_cat == VIDEO_ES )
        p_sys->i_video_id++;

    /* ESs added while a part is already running join it right away */
    if( p_sys->p_out )
        id->id = sout_StreamIdAdd( p_sys->p_out, &id->fmt, id->es_id );

    return id;
}

static void Del( sout_stream_t *p_stream, void *_id )
{
    sout_stream_sys_t *p_sys = p_stream->p_sys;
    sout_stream_id_sys_t *id = (sout_stream_id_sys_t *)_id;

    if( id->p_first )
        block_ChainRelease( id->p_first );
    id->p_first = NULL;
    id->pp_last = &id->p_first;

    if( id->fmt.i_cat == VIDEO_ES )
        p_sys->i_video_id--;

    assert( !id->id || p_sys->p_out );
    if( id->id )
        sout_StreamIdDel( p_sys->p_out, id->id );

    es_format_Clean( &id->fmt );
    TAB_REMOVE( p_sys->i_id, p_sys->id, id );

    free( id );
}

/*****************************************************************************
 * Output file naming
 *****************************************************************************/
static char *GetOutputFilename( sout_stream_t *p_stream, unsigned i_part )
{
    sout_stream_sys_t *p_sys = p_stream->p_sys;
    char *psz_file;

    /* A %d in the destination is replaced by the part number */
    if( strstr( p_sys->psz_dst, "%d" ) )
    {
        if( asprintf( &psz_file, p_sys->psz_dst, i_part ) < 0 )
            return NULL;
        return psz_file;
    }

    /* Otherwise, insert "-NNN" before the extension of the destination */
    const char *psz_ext = strrchr( p_sys->psz_dst, '.' );
    if( psz_ext && !strchr( psz_ext, '/' ) && !strchr( psz_ext, '\\' ) )
    {
        if( asprintf( &psz_file, "%.*s-%03u%s", (int)(psz_ext - p_sys->psz_dst),
                      p_sys->psz_dst, i_part, psz_ext ) < 0 )
            return NULL;
    }
    else
    {
        if( asprintf( &psz_file, "%s-%03u%s", p_sys->psz_dst, i_part,
                      p_sys->psz_ext ) < 0 )
            return NULL;
    }
    return psz_file;
}

/*****************************************************************************
 * Output chain handling
 *****************************************************************************/
static int OutputNew( sout_stream_t *p_stream, char **ppsz_file )
{
    sout_stream_sys_t *p_sys = p_stream->p_sys;
    char *psz_file = NULL, *psz_escaped = NULL;
    char *psz_output = NULL;
    int i_count = 0;

    psz_file = GetOutputFilename( p_stream, p_sys->i_part );
    if( !psz_file )
        goto error;

    psz_escaped = config_StringEscape( psz_file );
    if( !psz_escaped )
        goto error;

    if( asprintf( &psz_output,
                  "std{access=file{no-append,no-format,no-overwrite},"
                  "mux=%s,dst='%s'}", p_sys->psz_mux, psz_escaped ) < 0 )
    {
        psz_output = NULL;
        goto error;
    }

    msg_Dbg( p_stream, "starting part %u in `%s'", p_sys->i_part, psz_file );

    p_sys->p_out = sout_StreamChainNew( VLC_OBJECT(p_stream), psz_output,
                                        NULL );
    if( !p_sys->p_out )
        goto error;

    for( int i = 0; i < p_sys->i_id; i++ )
    {
        sout_stream_id_sys_t *id = p_sys->id[i];

        id->id = sout_StreamIdAdd( p_sys->p_out, &id->fmt, id->es_id );
        if( id->id )
        {
            i_count++;
            /* Every new file must start on a keyframe for video */
            id->b_wait_key = true;
        }
    }

    *ppsz_file = psz_file;
    free( psz_escaped );
    free( psz_output );

    return i_count;

error:
    free( psz_file );
    free( psz_escaped );
    free( psz_output );
    return -1;
}

static void OutputDelete( sout_stream_t *p_stream )
{
    sout_stream_sys_t *p_sys = p_stream->p_sys;

    for( int i = 0; i < p_sys->i_id; i++ )
    {
        sout_stream_id_sys_t *id = p_sys->id[i];

        if( id->id )
        {
            sout_StreamIdDel( p_sys->p_out, id->id );
            id->id = NULL;
        }
    }
    sout_StreamChainDelete( p_sys->p_out, NULL );
    p_sys->p_out = NULL;
}

/* Start (or restart) the output chain, writing to a new file */
static void OutputStart( sout_stream_t *p_stream )
{
    sout_stream_sys_t *p_sys = p_stream->p_sys;
    char *psz_file = NULL;

    /* Finish the current part first, so that its file is closed and its
     * stream ids are released before the next part is created */
    if( p_sys->p_out )
        OutputDelete( p_stream );

    p_sys->i_part++;
    if( OutputNew( p_stream, &psz_file ) < 0 )
    {
        msg_Err( p_stream, "failed to open output for part %u", p_sys->i_part );
        p_sys->p_out = NULL;
        p_sys->b_failed = true;
    }
    free( psz_file );
}

/* Flush the per-ES buffers into the freshly (re)started output chain */
static void FlushBuffered( sout_stream_t *p_stream )
{
    sout_stream_sys_t *p_sys = p_stream->p_sys;

    for( int i = 0; i < p_sys->i_id; i++ )
    {
        sout_stream_id_sys_t *id = p_sys->id[i];

        block_t *p_block = id->p_first;
        id->p_first = NULL;
        id->pp_last = &id->p_first;

        while( p_block )
        {
            block_t *p_next = p_block->p_next;
            p_block->p_next = NULL;
            OutputSend( p_stream, id, p_block );
            p_block = p_next;
        }
    }
}

/* Close the current part and open a new one with the buffered data */
static void SplitOutput( sout_stream_t *p_stream )
{
    OutputStart( p_stream );
    FlushBuffered( p_stream );
}

static vlc_tick_t BlockTick( const block_t *p_block )
{
    if( unlikely(!p_block) )
        return VLC_TICK_INVALID;
    else if( likely(p_block->i_dts != VLC_TICK_INVALID) )
        return p_block->i_dts;
    else
        return p_block->i_pts;
}

static void OutputSend( sout_stream_t *p_stream, sout_stream_id_sys_t *id, block_t *p_block )
{
    sout_stream_sys_t *p_sys = p_stream->p_sys;

    if( id->id )
    {
        /* We wait until the first keyframe for video streams (streams
         * without any block type information are sent right away) */
        if( unlikely( id->b_wait_key ) )
        {
            if( p_block->i_flags & BLOCK_FLAG_TYPE_I )
                id->b_wait_key = false;

            if( ( p_block->i_flags & BLOCK_FLAG_TYPE_MASK ) == 0 )
                id->b_wait_key = false;

            if( id->b_wait_key )
            {
                msg_Dbg( p_stream, "dropping block waiting for keyframe (dts %"PRId64")",
                         BlockTick( p_block ) );
                block_Release( p_block );
                return;
            }
        }
        sout_StreamIdSend( p_sys->p_out, id->id, p_block );
    }
    else
    {
        block_Release( p_block );
    }
}

static int Send( sout_stream_t *p_stream, void *_id, block_t *p_buffer )
{
    sout_stream_sys_t *p_sys = p_stream->p_sys;
    sout_stream_id_sys_t *id = (sout_stream_id_sys_t *)_id;

    if( p_sys->b_failed )
    {
        block_Release( p_buffer );
        return VLC_SUCCESS;
    }

    if( p_sys->p_out == NULL || p_sys->b_split_pending )
    {
        bool b_start_split = p_sys->b_split_pending &&
                             p_sys->i_video_id == 0;

        /* Buffer the block while the output chain is not started or a
         * chapter split is waiting for a video keyframe */
        block_ChainLastAppend( &id->pp_last, p_buffer );

        /* Without any video ES, a split can be applied immediately */
        if( b_start_split )
        {
            p_sys->b_split_pending = false;
            SplitOutput( p_stream );
        }
        else if( p_sys->p_out == NULL )
        {
            OutputStart( p_stream );
            FlushBuffered( p_stream );
            if( p_sys->p_out == NULL )
                p_sys->b_failed = true;
        }
        else if( id->fmt.i_cat == VIDEO_ES &&
                 ( ( p_buffer->i_flags & BLOCK_FLAG_TYPE_I ) ||
                   ( p_buffer->i_flags & BLOCK_FLAG_TYPE_MASK ) == 0 ) )
        {
            /* The split is applied on the next video keyframe (or on any
             * video block when the stream carries no frame type info) */
            p_sys->b_split_pending = false;
            SplitOutput( p_stream );
        }
        return VLC_SUCCESS;
    }

    OutputSend( p_stream, id, p_buffer );

    return VLC_SUCCESS;
}

/*****************************************************************************
 * Control
 *****************************************************************************/
static int Control( sout_stream_t *p_stream, int i_query, va_list args )
{
    sout_stream_sys_t *p_sys = p_stream->p_sys;

    switch( i_query )
    {
        case SOUT_STREAM_CHAPTER_CHANGE:
        {
            int i_title = va_arg( args, int );
            int i_seekpoint = va_arg( args, int );

            /* A first notification received before any data was written only
             * marks the chapter the conversion starts in. */
            if( !p_sys->b_chapter_valid && p_sys->p_out == NULL )
            {
                p_sys->b_chapter_valid = true;
                p_sys->i_title = i_title;
                p_sys->i_seekpoint = i_seekpoint;
                return VLC_SUCCESS;
            }

            if( !p_sys->b_chapter_valid ||
                i_title != p_sys->i_title ||
                i_seekpoint != p_sys->i_seekpoint )
            {
                msg_Dbg( p_stream, "chapter change: title %d chapter %d",
                         i_title, i_seekpoint );
                p_sys->b_split_pending = true;
            }
            p_sys->b_chapter_valid = true;
            p_sys->i_title = i_title;
            p_sys->i_seekpoint = i_seekpoint;
            return VLC_SUCCESS;
        }
        case SOUT_STREAM_IS_SYNCHRONOUS:
        {
            if( p_sys->p_out )
                return sout_StreamControl( p_sys->p_out, i_query,
                                           va_arg( args, bool * ) );
            break;
        }
    }
    return VLC_EGENERIC;
}

static void SetPCR( sout_stream_t *p_stream, vlc_tick_t pcr )
{
    sout_stream_sys_t *p_sys = p_stream->p_sys;

    if( p_sys->p_out )
        sout_StreamSetPCR( p_sys->p_out, pcr );
}
