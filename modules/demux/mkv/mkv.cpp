/*****************************************************************************
 * mkv.cpp : matroska demuxer
 *****************************************************************************
 * Copyright (C) 2003-2005, 2008, 2010 VLC authors and VideoLAN
 * $Id$
 *
 * Authors: Laurent Aimar <fenrir@via.ecp.fr>
 *          Steve Lhomme <steve.lhomme@free.fr>
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

#include "mkv.hpp"
#include "util.hpp"

#include "matroska_segment.hpp"
#include "demux.hpp"

#include "chapters.hpp"
#include "Ebml_parser.hpp"

#include "stream_io_callback.hpp"

#include <new>

extern "C" {
#include "../../modules/codec/dts_header.h"
}

#include <vlc_fs.h>
#include <vlc_url.h>

/*****************************************************************************
 * Module descriptor
 *****************************************************************************/
static int  Open ( vlc_object_t * );
static void Close( vlc_object_t * );

vlc_module_begin ()
    set_shortname( "Matroska" )
    set_description( N_("Matroska stream demuxer" ) )
    set_capability( "demux", 50 )
    set_callbacks( Open, Close )
    set_category( CAT_INPUT )
    set_subcategory( SUBCAT_INPUT_DEMUX )

    add_bool( "mkv-use-ordered-chapters", true,
            N_("Respect ordered chapters"),
            N_("Play chapters in the order specified in the segment."), false );

    add_bool( "mkv-use-chapter-codec", true,
            N_("Chapter codecs"),
            N_("Use chapter codecs found in the segment."), true );

    add_bool( "mkv-preload-local-dir", true,
            N_("Preload MKV files in the same directory"),
            N_("Preload matroska files in the same directory to find linked segments (not good for broken files)."), false );

    add_bool( "mkv-seek-percent", false,
            N_("Seek based on percent not time"),
            N_("Seek based on percent not time."), true );

    add_bool( "mkv-use-dummy", false,
            N_("Dummy Elements"),
            N_("Read and discard unknown EBML elements (not good for broken files)."), true );

    add_shortcut( "mka", "mkv" )
vlc_module_end ()

struct demux_sys_t;

static int  Demux  ( demux_t * );
static int  Control( demux_t *, int, va_list );
static void Seek   ( demux_t *, mtime_t i_mk_date, double f_percent, virtual_chapter_c *p_vchapter );

/*****************************************************************************
 * Open: initializes matroska demux structures
 *****************************************************************************/
static int Open( vlc_object_t * p_this )
{
    demux_t            *p_demux = (demux_t*)p_this;
    demux_sys_t        *p_sys;
    matroska_stream_c  *p_stream;
    matroska_segment_c *p_segment;
    const uint8_t      *p_peek;
    std::string         s_path, s_filename;
    vlc_stream_io_callback *p_io_callback;
    EbmlStream         *p_io_stream;
    bool                b_need_preload = false;

    /* peek the begining */
    if( stream_Peek( p_demux->s, &p_peek, 4 ) < 4 ) return VLC_EGENERIC;

    /* is a valid file */
    if( p_peek[0] != 0x1a || p_peek[1] != 0x45 ||
        p_peek[2] != 0xdf || p_peek[3] != 0xa3 ) return VLC_EGENERIC;

    /* Set the demux function */
    p_demux->pf_demux   = Demux;
    p_demux->pf_control = Control;
    p_demux->p_sys      = p_sys = new demux_sys_t( *p_demux );

    p_io_callback = new vlc_stream_io_callback( p_demux->s, false );
    p_io_stream = new (std::nothrow) EbmlStream( *p_io_callback );

    if( p_io_stream == NULL )
    {
        msg_Err( p_demux, "failed to create EbmlStream" );
        delete p_io_callback;
        delete p_sys;
        return VLC_EGENERIC;
    }

    p_stream = p_sys->AnalyseAllSegmentsFound( p_demux, p_io_stream, true );
    if( p_stream == NULL )
    {
        msg_Err( p_demux, "cannot find KaxSegment or missing mandatory KaxInfo" );
        goto error;
    }
    p_sys->streams.push_back( p_stream );

    p_stream->p_io_callback = p_io_callback;
    p_stream->p_estream = p_io_stream;

    for (size_t i=0; i<p_stream->segments.size(); i++)
    {
        p_stream->segments[i]->Preload();
        b_need_preload |= p_stream->segments[i]->b_ref_external_segments;
        if ( p_stream->segments[i]->translations.size() &&
             p_stream->segments[i]->translations[0]->codec_id == MATROSKA_CHAPTER_CODEC_DVD &&
             p_stream->segments[i]->families.size() )
            b_need_preload = true;
    }

    p_segment = p_stream->segments[0];
    if( p_segment->cluster == NULL && p_segment->stored_editions.size() == 0 )
    {
        msg_Err( p_demux, "cannot find any cluster or chapter, damaged file ?" );
        goto error;
    }

    if (b_need_preload && var_InheritBool( p_demux, "mkv-preload-local-dir" ))
    {
        msg_Dbg( p_demux, "Preloading local dir" );
        /* get the files from the same dir from the same family (based on p_demux->psz_path) */
        if ( p_demux->psz_file && !strcmp( p_demux->psz_access, "file" ) )
        {
            // assume it's a regular file
            // get the directory path
            s_path = p_demux->psz_file;
            if (s_path.at(s_path.length() - 1) == DIR_SEP_CHAR)
            {
                s_path = s_path.substr(0,s_path.length()-1);
            }
            else
            {
                if (s_path.find_last_of(DIR_SEP_CHAR) > 0)
                {
                    s_path = s_path.substr(0,s_path.find_last_of(DIR_SEP_CHAR));
                }
            }

            DIR *p_src_dir = vlc_opendir(s_path.c_str());

            if (p_src_dir != NULL)
            {
                const char *psz_file;
                while ((psz_file = vlc_readdir(p_src_dir)) != NULL)
                {
                    if (strlen(psz_file) > 4)
                    {
                        s_filename = s_path + DIR_SEP_CHAR + psz_file;

#if defined(_WIN32) || defined(__OS2__)
                        if (!strcasecmp(s_filename.c_str(), p_demux->psz_file))
#else
                        if (!s_filename.compare(p_demux->psz_file))
#endif
                        {
                            continue; // don't reuse the original opened file
                        }

                        if (!s_filename.compare(s_filename.length() - 3, 3, "mkv") ||
                            !s_filename.compare(s_filename.length() - 3, 3, "mka"))
                        {
                            // test whether this file belongs to our family
                            const uint8_t *p_peek;
                            bool          file_ok = false;
                            char          *psz_url = vlc_path2uri( s_filename.c_str(), "file" );
                            stream_t      *p_file_stream = stream_UrlNew(
                                                            p_demux,
                                                            psz_url );
                            /* peek the begining */
                            if( p_file_stream &&
                                stream_Peek( p_file_stream, &p_peek, 4 ) >= 4
                                && p_peek[0] == 0x1a && p_peek[1] == 0x45 &&
                                p_peek[2] == 0xdf && p_peek[3] == 0xa3 ) file_ok = true;

                            if ( file_ok )
                            {
                                vlc_stream_io_callback *p_file_io = new vlc_stream_io_callback( p_file_stream, true );
                                EbmlStream *p_estream = new EbmlStream(*p_file_io);

                                p_stream = p_sys->AnalyseAllSegmentsFound( p_demux, p_estream );

                                if ( p_stream == NULL )
                                {
                                    msg_Dbg( p_demux, "the file '%s' will not be used", s_filename.c_str() );
                                    delete p_estream;
                                    delete p_file_io;
                                }
                                else
                                {
                                    p_stream->p_io_callback = p_file_io;
                                    p_stream->p_estream = p_estream;
                                    p_sys->streams.push_back( p_stream );
                                }
                            }
                            else
                            {
                                if( p_file_stream ) {
                                    stream_Delete( p_file_stream );
                                }
                                msg_Dbg( p_demux, "the file '%s' cannot be opened", s_filename.c_str() );
                            }
                            free( psz_url );
                        }
                    }
                }
                closedir( p_src_dir );
            }
        }

        p_sys->PreloadFamily( *p_segment );
    }
    else if (b_need_preload)
        msg_Warn( p_demux, "This file references other files, you may want to enable the preload of local directory");

    if ( !p_sys->PreloadLinked() ||
         !p_sys->PreparePlayback( *p_sys->p_current_vsegment, 0 ) )
    {
        msg_Err( p_demux, "cannot use the segment" );
        goto error;
    }

    p_sys->FreeUnused();

    p_sys->InitUi();

    return VLC_SUCCESS;

error:
    delete p_sys;
    return VLC_EGENERIC;
}

/*****************************************************************************
 * Close: frees unused data
 *****************************************************************************/
static void Close( vlc_object_t *p_this )
{
    demux_t     *p_demux = reinterpret_cast<demux_t*>( p_this );
    demux_sys_t *p_sys   = p_demux->p_sys;
    virtual_segment_c *p_vsegment = p_sys->p_current_vsegment;
    if( p_vsegment )
    {
        matroska_segment_c *p_segment = p_vsegment->CurrentSegment();
        if( p_segment )
            p_segment->UnSelect();
    }

    delete p_sys;
}

/*****************************************************************************
 * Control:
 *****************************************************************************/
static int Control( demux_t *p_demux, int i_query, va_list args )
{
    demux_sys_t        *p_sys = p_demux->p_sys;
    int64_t     *pi64, i64;
    double      *pf, f;
    int         i_skp;
    size_t      i_idx;

    vlc_meta_t *p_meta;
    input_attachment_t ***ppp_attach;
    int *pi_int;

    switch( i_query )
    {
        case DEMUX_CAN_SEEK:
            return stream_vaControl( p_demux->s, i_query, args );

        case DEMUX_GET_ATTACHMENTS:
            ppp_attach = va_arg( args, input_attachment_t*** );
            pi_int = va_arg( args, int * );

            if( p_sys->stored_attachments.size() <= 0 )
                return VLC_EGENERIC;

            *pi_int = p_sys->stored_attachments.size();
            *ppp_attach = static_cast<input_attachment_t**>( malloc( sizeof(input_attachment_t*) *
                                                        p_sys->stored_attachments.size() ) );
            if( !(*ppp_attach) )
                return VLC_ENOMEM;
            for( size_t i = 0; i < p_sys->stored_attachments.size(); i++ )
            {
                attachment_c *a = p_sys->stored_attachments[i];
                (*ppp_attach)[i] = vlc_input_attachment_New( a->fileName(), a->mimeType(), NULL,
                                                             a->p_data, a->size() );
                if( !(*ppp_attach)[i] )
                {
                    free(*ppp_attach);
                    return VLC_ENOMEM;
                }
            }
            return VLC_SUCCESS;

        case DEMUX_GET_META:
            p_meta = va_arg( args, vlc_meta_t* );
            vlc_meta_Merge( p_meta, p_sys->meta );
            return VLC_SUCCESS;

        case DEMUX_GET_LENGTH:
            pi64 = va_arg( args, int64_t * );
            if( p_sys->f_duration > 0.0 )
            {
                *pi64 = static_cast<int64_t>( p_sys->f_duration * 1000 );
                return VLC_SUCCESS;
            }
            return VLC_EGENERIC;

        case DEMUX_GET_POSITION:
            pf = va_arg( args, double * );
            if ( p_sys->f_duration > 0.0 )
                *pf = static_cast<double> (p_sys->i_pcr >= p_sys->i_start_pts ? p_sys->i_pcr : p_sys->i_start_pts ) / (1000.0 * p_sys->f_duration);
            return VLC_SUCCESS;

        case DEMUX_SET_POSITION:
            if( p_sys->f_duration > 0.0 )
            {
                f = va_arg( args, double );
                Seek( p_demux, -1, f, NULL );
                return VLC_SUCCESS;
            }
            return VLC_EGENERIC;

        case DEMUX_GET_TIME:
            pi64 = va_arg( args, int64_t * );
            *pi64 = p_sys->i_pcr;
            return VLC_SUCCESS;

        case DEMUX_GET_TITLE_INFO:
            if( p_sys->titles.size() > 1 || ( p_sys->titles.size() == 1 && p_sys->titles[0]->i_seekpoint > 0 ) )
            {
                input_title_t ***ppp_title = va_arg( args, input_title_t*** );
                int *pi_int = va_arg( args, int* );

                *pi_int = p_sys->titles.size();
                *ppp_title = static_cast<input_title_t**>( malloc( sizeof( input_title_t* ) * p_sys->titles.size() ) );

                for( size_t i = 0; i < p_sys->titles.size(); i++ )
                    (*ppp_title)[i] = vlc_input_title_Duplicate( p_sys->titles[i] );
                return VLC_SUCCESS;
            }
            return VLC_EGENERIC;

        case DEMUX_SET_TITLE:
            /* handle editions as titles */
            i_idx = va_arg( args, int );
            if(i_idx <  p_sys->titles.size() && p_sys->titles[i_idx]->i_seekpoint)
            {
                p_sys->p_current_vsegment->i_current_edition = i_idx;
                p_sys->i_current_title = i_idx;
                p_sys->p_current_vsegment->p_current_vchapter = p_sys->p_current_vsegment->veditions[p_sys->p_current_vsegment->i_current_edition]->getChapterbyTimecode(0);

                Seek( p_demux, static_cast<int64_t>( p_sys->titles[i_idx]->seekpoint[0]->i_time_offset ), -1, NULL);
                p_demux->info.i_update |= INPUT_UPDATE_SEEKPOINT|INPUT_UPDATE_TITLE;
                p_demux->info.i_seekpoint = 0;
                p_demux->info.i_title = i_idx;
                p_sys->f_duration = (float) p_sys->titles[i_idx]->i_length / 1000.f;
                return VLC_SUCCESS;
            }
            return VLC_EGENERIC;

        case DEMUX_SET_SEEKPOINT:
            i_skp = va_arg( args, int );

            // TODO change the way it works with the << & >> buttons on the UI (+1/-1 instead of a number)
            if( p_sys->titles.size() && i_skp < p_sys->titles[p_sys->i_current_title]->i_seekpoint)
            {
                Seek( p_demux, static_cast<int64_t>( p_sys->titles[p_sys->i_current_title]->seekpoint[i_skp]->i_time_offset ), -1, NULL);
                p_demux->info.i_update |= INPUT_UPDATE_SEEKPOINT;
                p_demux->info.i_seekpoint = i_skp;
                return VLC_SUCCESS;
            }
            return VLC_EGENERIC;

        case DEMUX_GET_FPS:
            pf = va_arg( args, double * );
            *pf = 0.0;
            if( p_sys->p_current_vsegment && p_sys->p_current_vsegment->CurrentSegment() )
            {
                const matroska_segment_c *p_segment = p_sys->p_current_vsegment->CurrentSegment();
                for( size_t i = 0; i < p_segment->tracks.size(); i++ )
                {
                    mkv_track_t *tk = p_segment->tracks[i];
                    if( tk->fmt.i_cat == VIDEO_ES && tk->fmt.video.i_frame_rate_base > 0 )
                    {
                        *pf = (double)tk->fmt.video.i_frame_rate / tk->fmt.video.i_frame_rate_base;
                        break;
                    }
                }
            }
            return VLC_SUCCESS;

        case DEMUX_SET_TIME:
            i64 = va_arg( args, int64_t );
            msg_Dbg(p_demux,"SET_TIME to %" PRId64, i64 );
            Seek( p_demux, i64, -1, NULL );
            return VLC_SUCCESS;
        default:
            return VLC_EGENERIC;
    }
}

/* Seek */
static void Seek( demux_t *p_demux, mtime_t i_mk_date, double f_percent, virtual_chapter_c *p_vchapter )
{
    demux_sys_t        *p_sys = p_demux->p_sys;
    virtual_segment_c  *p_vsegment = p_sys->p_current_vsegment;
    matroska_segment_c *p_segment = p_vsegment->CurrentSegment();
    int64_t            i_global_position = -1;

    if( f_percent < 0 ) msg_Dbg( p_demux, "seek request to i_pos = %" PRId64, i_mk_date );
    else                msg_Dbg( p_demux, "seek request to %.2f%%", f_percent * 100 );

    if( i_mk_date < 0 && f_percent < 0 )
    {
        msg_Warn( p_demux, "cannot seek nowhere!" );
        return;
    }
    if( f_percent > 1.0 )
    {
        msg_Warn( p_demux, "cannot seek so far!" );
        return;
    }
    if( p_sys->f_duration < 0 )
    {
        msg_Warn( p_demux, "cannot seek without duration!");
        return;
    }
    if( !p_segment )
    {
        msg_Warn( p_demux, "cannot seek without valid segment position");
        return;
    }

    /* seek without index or without date */
    if( f_percent >= 0 && (var_InheritBool( p_demux, "mkv-seek-percent" ) || !p_segment->b_cues || i_mk_date < 0 ))
    {
        i_mk_date = int64_t( f_percent * p_sys->f_duration * 1000.0 );
        if( !p_segment->b_cues )
        {
            int64_t i_pos = int64_t( f_percent * stream_Size( p_demux->s ) );

            msg_Dbg( p_demux, "lengthy way of seeking for pos:%" PRId64, i_pos );

            if (p_segment->indexes.size())
            {
                matroska_segment_c::indexes_t::iterator it          = p_segment->indexes_begin ();
                matroska_segment_c::indexes_t::iterator last_active = p_segment->indexes_end ();

                for ( ; it != last_active; ++it )
                {
                    if( it->i_position >= i_pos && it->i_mk_time != -1 )
                        break;
                }

                if ( it == last_active && it != p_segment->indexes.begin() )
                    --it;

                if( it->i_position < i_pos )
                {
                    msg_Dbg( p_demux, "no cues, seek request to global pos: %" PRId64, i_pos );
                    i_global_position = i_pos;
                }
            }
        }
    }
    p_vsegment->Seek( *p_demux, i_mk_date, p_vchapter, i_global_position );
}

/* Needed by matroska_segment::Seek() and Seek */
void BlockDecode( demux_t *p_demux, KaxBlock *block, KaxSimpleBlock *simpleblock,
                  mtime_t i_pts, mtime_t i_duration, bool b_key_picture,
                  bool b_discardable_picture )
{
    demux_sys_t        *p_sys = p_demux->p_sys;
    matroska_segment_c *p_segment = p_sys->p_current_vsegment->CurrentSegment();

    if( !p_segment ) return;

    size_t          i_track;
    if( p_segment->BlockFindTrackIndex( &i_track, block, simpleblock ) )
    {
        msg_Err( p_demux, "invalid track number" );
        return;
    }

    mkv_track_t *tk = p_segment->tracks[i_track];

    if( tk->fmt.i_cat != NAV_ES && tk->p_es == NULL )
    {
        msg_Err( p_demux, "unknown track number" );
        return;
    }

    i_pts -= tk->i_codec_delay;

    if ( tk->fmt.i_cat != NAV_ES )
    {
        bool b;
        es_out_Control( p_demux->out, ES_OUT_GET_ES_STATE, tk->p_es, &b );

        if( !b )
        {
            tk->b_inited = false;
            if( tk->fmt.i_cat == VIDEO_ES || tk->fmt.i_cat == AUDIO_ES )
                tk->i_last_dts = VLC_TS_INVALID;
            return;
        }
    }


    /* First send init data */
    if( !tk->b_inited && tk->i_data_init > 0 )
    {
        block_t *p_init;

        msg_Dbg( p_demux, "sending header (%d bytes)", tk->i_data_init );
        p_init = MemToBlock( tk->p_data_init, tk->i_data_init, 0 );
        if( p_init ) send_Block( p_demux, tk, p_init, 1, 0 );
    }
    tk->b_inited = true;


    size_t frame_size = 0;
    size_t block_size = 0;

    if( simpleblock != NULL )
        block_size = simpleblock->GetSize();
    else
        block_size = block->GetSize();
 
    const unsigned int i_number_frames = block != NULL ? block->NumberFrames() :
            ( simpleblock != NULL ? simpleblock->NumberFrames() : 0 );
    for( unsigned int i_frame = 0; i_frame < i_number_frames; i_frame++ )
    {
        block_t *p_block;
        DataBuffer *data;
        if( simpleblock != NULL )
        {
            data = &simpleblock->GetBuffer(i_frame);
        }
        else
        {
            data = &block->GetBuffer(i_frame);
        }
        frame_size += data->Size();
        if( !data->Buffer() || data->Size() > frame_size || frame_size > block_size  )
        {
            msg_Warn( p_demux, "Cannot read frame (too long or no frame)" );
            break;
        }

        if( tk->i_compression_type == MATROSKA_COMPRESSION_HEADER &&
            tk->p_compression_data != NULL &&
            tk->i_encoding_scope & MATROSKA_ENCODING_SCOPE_ALL_FRAMES )
            p_block = MemToBlock( data->Buffer(), data->Size(), tk->p_compression_data->GetSize() );
        else if( unlikely( tk->fmt.i_codec == VLC_CODEC_WAVPACK ) )
            p_block = packetize_wavpack(tk, data->Buffer(), data->Size());
        else
            p_block = MemToBlock( data->Buffer(), data->Size(), 0 );

        if( p_block == NULL )
        {
            break;
        }

#if defined(HAVE_ZLIB_H)
        if( tk->i_compression_type == MATROSKA_COMPRESSION_ZLIB &&
            tk->i_encoding_scope & MATROSKA_ENCODING_SCOPE_ALL_FRAMES )
        {
            p_block = block_zlib_decompress( VLC_OBJECT(p_demux), p_block );
            if( p_block == NULL )
                break;
        }
        else
#endif
        if( tk->i_compression_type == MATROSKA_COMPRESSION_HEADER &&
            tk->i_encoding_scope & MATROSKA_ENCODING_SCOPE_ALL_FRAMES )
        {
            memcpy( p_block->p_buffer, tk->p_compression_data->GetBuffer(), tk->p_compression_data->GetSize() );
        }

        if ( b_key_picture )
            p_block->i_flags |= BLOCK_FLAG_TYPE_I;

        switch( tk->fmt.i_codec )
        {
        case VLC_CODEC_COOK:
        case VLC_CODEC_ATRAC3:
        {
            handle_real_audio(p_demux, tk, p_block, i_pts);
            block_Release(p_block);
            i_pts = ( tk->i_default_duration )?
                i_pts + ( mtime_t )tk->i_default_duration:
                VLC_TS_INVALID;
            continue;
         }

        case VLC_CODEC_DTS:
            /* Check if packetization is correct and without padding.
             * example: Test_mkv_div3_DTS_1920x1080_1785Kbps_23,97fps.mkv */
            if( p_block->i_buffer > 6 )
            {
                unsigned int a, b, c, d;
                bool e;
                int i_frame_size = GetSyncInfo( p_block->p_buffer, &e, &a, &b, &c, &d );
                if( i_frame_size > 0 )
                    p_block->i_buffer = __MIN(p_block->i_buffer, (size_t)i_frame_size);
            }
            break;

         case VLC_CODEC_OPUS:
            mtime_t i_length = i_duration * tk-> f_timecodescale *
                    (double) p_segment->i_timescale / 1000.0;
            if ( i_length < 0 ) i_length = 0;
            p_block->i_nb_samples = i_length * tk->fmt.audio.i_rate
                    / CLOCK_FREQ;
            break;
        }

        if( tk->fmt.i_cat != VIDEO_ES )
        {
            if ( tk->fmt.i_cat == NAV_ES )
            {
                // TODO handle the start/stop times of this packet
                p_sys->p_ev->SetPci( (const pci_t *)&p_block->p_buffer[1]);
                block_Release( p_block );
                return;
            }
            p_block->i_dts = p_block->i_pts = i_pts;
        }
        else
        {
            // correct timestamping when B frames are used
            if( tk->b_dts_only )
            {
                p_block->i_pts = VLC_TS_INVALID;
                p_block->i_dts = i_pts;
            }
            else if( tk->b_pts_only )
            {
                p_block->i_pts = i_pts;
                p_block->i_dts = i_pts;
            }
            else
            {
                p_block->i_pts = i_pts;
                // condition when the DTS is correct (keyframe or B frame == NOT P frame)
                if ( b_key_picture || b_discardable_picture )
                    p_block->i_dts = p_block->i_pts;
                else if ( tk->i_last_dts == VLC_TS_INVALID )
                    p_block->i_dts = i_pts;
                else
                    p_block->i_dts = std::min( i_pts, tk->i_last_dts + ( mtime_t )tk->i_default_duration );
            }
        }

        send_Block( p_demux, tk, p_block, i_number_frames, i_duration );

        /* use time stamp only for first block */
        i_pts = ( tk->i_default_duration )?
                 i_pts + ( mtime_t )tk->i_default_duration:
                 ( tk->fmt.b_packetized ) ? VLC_TS_INVALID : i_pts + 1;
    }
}

/*****************************************************************************
 * Demux: reads and demuxes data packets
 *****************************************************************************
 * Returns -1 in case of error, 0 in case of EOF, 1 otherwise
 *****************************************************************************/
static int Demux( demux_t *p_demux)
{
    demux_sys_t        *p_sys = p_demux->p_sys;

    vlc_mutex_locker demux_lock ( &p_sys->lock_demuxer );

    virtual_segment_c  *p_vsegment = p_sys->p_current_vsegment;

    if( p_sys->i_pts >= p_sys->i_start_pts )
    {
        if ( p_vsegment->UpdateCurrentToChapter( *p_demux ) )
            return 1;
        p_vsegment = p_sys->p_current_vsegment;
    }

    matroska_segment_c *p_segment = p_vsegment->CurrentSegment();
    if ( p_segment == NULL )
        return 0;

    KaxBlock *block;
    KaxSimpleBlock *simpleblock;
    int64_t i_block_duration = 0;
    bool b_key_picture;
    bool b_discardable_picture;
    if( p_segment->BlockGet( block, simpleblock, &b_key_picture, &b_discardable_picture, &i_block_duration ) )
    {
        if ( p_vsegment->CurrentEdition() && p_vsegment->CurrentEdition()->b_ordered )
        {
            const virtual_chapter_c *p_chap = p_vsegment->CurrentChapter();
            // check if there are more chapters to read
            if ( p_chap != NULL )
            {
                /* TODO handle successive chapters with the same user_start_time/user_end_time
                */
                p_sys->i_pts = p_chap->i_mk_virtual_stop_time + VLC_TS_0;
                p_sys->i_pts++; // trick to avoid staying on segments with no duration and no content

                return 1;
            }
        }

        msg_Warn( p_demux, "cannot get block EOF?" );
        return 0;
    }

    if( simpleblock != NULL )
        p_sys->i_pts = (mtime_t)simpleblock->GlobalTimecode() / INT64_C(1000);
    else
        p_sys->i_pts = (mtime_t)block->GlobalTimecode() / INT64_C(1000);
    p_sys->i_pts += p_sys->i_mk_chapter_time + VLC_TS_0;

    mtime_t i_pcr = VLC_TS_INVALID;
    for( size_t i = 0; i < p_segment->tracks.size(); i++)
        if( p_segment->tracks[i]->i_last_dts > VLC_TS_INVALID &&
            ( p_segment->tracks[i]->i_last_dts < i_pcr || i_pcr == VLC_TS_INVALID ))
            i_pcr = p_segment->tracks[i]->i_last_dts;

    if( i_pcr > p_sys->i_pcr + 300000 )
    {
        es_out_Control( p_demux->out, ES_OUT_SET_PCR, VLC_TS_0 + p_sys->i_pcr );
        p_sys->i_pcr = i_pcr;
    }

    if ( p_vsegment->CurrentEdition() &&
         p_vsegment->CurrentEdition()->b_ordered &&
         p_vsegment->CurrentChapter() == NULL )
    {
        /* nothing left to read in this ordered edition */
        delete block;
        return 0;
    }

    BlockDecode( p_demux, block, simpleblock, p_sys->i_pts, i_block_duration, b_key_picture, b_discardable_picture );

    delete block;

    return 1;
}
