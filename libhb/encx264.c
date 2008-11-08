/* $Id: encx264.c,v 1.21 2005/11/04 13:09:41 titer Exp $

   This file is part of the HandBrake source code.
   Homepage: <http://handbrake.fr/>.
   It may be used under the terms of the GNU General Public License. */

#include <stdarg.h>

#include "hb.h"

#include "x264.h"

int  encx264Init( hb_work_object_t *, hb_job_t * );
int  encx264Work( hb_work_object_t *, hb_buffer_t **, hb_buffer_t ** );
void encx264Close( hb_work_object_t * );

hb_work_object_t hb_encx264 =
{
    WORK_ENCX264,
    "H.264/AVC encoder (libx264)",
    encx264Init,
    encx264Work,
    encx264Close
};

#define DTS_BUFFER_SIZE 32

/*
 * The frame info struct remembers information about each frame across calls
 * to x264_encoder_encode. Since frames are uniquely identified by their
 * timestamp, we use some bits of the timestamp as an index. The LSB is
 * chosen so that two successive frames will have different values in the
 * bits over any plausible range of frame rates. (Starting with bit 8 allows
 * any frame rate slower than 352fps.) The MSB determines the size of the array.
 * It is chosen so that two frames can't use the same slot during the
 * encoder's max frame delay (set by the standard as 16 frames) and so
 * that, up to some minimum frame rate, frames are guaranteed to map to
 * different slots. (An MSB of 17 which is 2^(17-8+1) = 1024 slots guarantees
 * no collisions down to a rate of .7 fps).
 */
#define FRAME_INFO_MAX2 (8)     // 2^8 = 256; 90000/256 = 352 frames/sec
#define FRAME_INFO_MIN2 (17)    // 2^17 = 128K; 90000/131072 = 1.4 frames/sec
#define FRAME_INFO_SIZE (1 << (FRAME_INFO_MIN2 - FRAME_INFO_MAX2 + 1))
#define FRAME_INFO_MASK (FRAME_INFO_SIZE - 1)

struct hb_work_private_s
{
    hb_job_t       * job;
    x264_t         * x264;
    x264_picture_t   pic_in;
    uint8_t         *x264_allocated_pic;

    int            chap_mark;   // saved chap mark when we're propagating it
    int64_t        last_stop;   // Debugging - stop time of previous input frame
    int64_t        init_delay;
    int64_t        next_chap;

    struct {
        int64_t duration;
    } frame_info[FRAME_INFO_SIZE];

    char             filename[1024];
};

/***********************************************************************
 * hb_work_encx264_init
 ***********************************************************************
 *
 **********************************************************************/
int encx264Init( hb_work_object_t * w, hb_job_t * job )
{
    x264_param_t       param;
    x264_nal_t       * nal;
    int                nal_count;
    int                nal_size;

    hb_work_private_t * pv = calloc( 1, sizeof( hb_work_private_t ) );
    w->private_data = pv;

    pv->job = job;

    memset( pv->filename, 0, 1024 );
    hb_get_tempory_filename( job->h, pv->filename, "x264.log" );

    x264_param_default( &param );

    param.i_threads    = ( hb_get_cpu_count() * 3 / 2 );
    param.i_width      = job->width;
    param.i_height     = job->height;
    param.i_fps_num    = job->vrate;
    param.i_fps_den    = job->vrate_base;

    if (job->vrate_base != 1080000)
    {
        /* If the fps isn't 25, adjust the key intervals. Add 1 because
           we want 24, not 23 with a truncated remainder.               */
        param.i_keyint_min     = (job->vrate / job->vrate_base) + 1;
        param.i_keyint_max = (10 * job->vrate / job->vrate_base) + 1;
        hb_log("encx264: keyint-min: %i, keyint-max: %i", param.i_keyint_min, param.i_keyint_max);
    }

    param.i_log_level  = X264_LOG_INFO;
    if( job->h264_level )
    {
        param.b_cabac     = 0;
        param.i_level_idc = job->h264_level;
        hb_log( "encx264: encoding at level %i",
                param.i_level_idc );
    }

    /*
       	This section passes the string x264opts to libx264 for parsing into
        parameter names and values.

        The string is set up like this:
        option1=value1:option2=value 2

        So, you have to iterate through based on the colons, and then put
        the left side of the equals sign in "name" and the right side into
        "value." Then you hand those strings off to x264 for interpretation.

        This is all based on the universal x264 option handling Loren
        Merritt implemented in the Mplayer/Mencoder project.
     */

    if( job->x264opts != NULL && *job->x264opts != '\0' )
    {
        char *x264opts, *x264opts_start;

        x264opts = x264opts_start = strdup(job->x264opts);

        while( x264opts_start && *x264opts )
        {
            char *name = x264opts;
            char *value;
            int ret;

            x264opts += strcspn( x264opts, ":" );
            if( *x264opts )
            {
                *x264opts = 0;
                x264opts++;
            }

            value = strchr( name, '=' );
            if( value )
            {
                *value = 0;
                value++;
            }

            /*
               When B-frames are enabled, the max frame count increments
               by 1 (regardless of the number of B-frames). If you don't
               change the duration of the video track when you mux, libmp4
               barfs.  So, check if the x264opts are using B-frames, and
               when they are, set the boolean job->areBframes as true.
             */

            if( !( strcmp( name, "bframes" ) ) )
            {
                if( atoi( value ) > 0 )
                {
                    job->areBframes = 1;
                }
            }

            /* Note b-pyramid here, so the initial delay can be doubled */
            if( !( strcmp( name, "b-pyramid" ) ) )
            {
                if( value != NULL )
                {
                    if( atoi( value ) > 0 )
                    {
                        job->areBframes = 2;
                    }
                }
                else
                {
                    job->areBframes = 2;
                }
            }

            /* Here's where the strings are passed to libx264 for parsing. */
            ret = x264_param_parse( &param, name, value );

            /* 	Let x264 sanity check the options for us*/
            if( ret == X264_PARAM_BAD_NAME )
                hb_log( "x264 options: Unknown suboption %s", name );
            if( ret == X264_PARAM_BAD_VALUE )
                hb_log( "x264 options: Bad argument %s=%s", name, value ? value : "(null)" );
        }
        free(x264opts_start);
    }

    /* set up the VUI color model & gamma to match what the COLR atom
     * set in muxmp4.c says. See libhb/muxmp4.c for notes. */
    if( job->color_matrix == 1 )
    {
        // ITU BT.601 DVD or SD TV content
        param.vui.i_colorprim = 6;
        param.vui.i_transfer = 1;
        param.vui.i_colmatrix = 6;
    }
    else if( job->color_matrix == 2 )
    {
        // ITU BT.709 HD content
        param.vui.i_colorprim = 1;
        param.vui.i_transfer = 1;
        param.vui.i_colmatrix = 1;
    }
    else if ( job->title->width >= 1280 || job->title->height >= 720 )
    {
        // we guess that 720p or above is ITU BT.709 HD content
        param.vui.i_colorprim = 1;
        param.vui.i_transfer = 1;
        param.vui.i_colmatrix = 1;
    }
    else
    {
        // ITU BT.601 DVD or SD TV content
        param.vui.i_colorprim = 6;
        param.vui.i_transfer = 1;
        param.vui.i_colmatrix = 6;
    }

    if( job->pixel_ratio )
    {
        param.vui.i_sar_width = job->pixel_aspect_width;
        param.vui.i_sar_height = job->pixel_aspect_height;

        hb_log( "encx264: encoding with stored aspect %d/%d",
                param.vui.i_sar_width, param.vui.i_sar_height );
    }


    if( job->vquality > 0.0 && job->vquality < 1.0 )
    {
        switch( job->crf )
        {
            case 1:
                /*Constant RF*/
                param.rc.i_rc_method = X264_RC_CRF;
                param.rc.f_rf_constant = 51 - job->vquality * 51;
                hb_log( "encx264: Encoding at constant RF %f",
                        param.rc.f_rf_constant );
                break;

            case 0:
                /*Constant QP*/
                param.rc.i_rc_method = X264_RC_CQP;
                param.rc.i_qp_constant = 51 - job->vquality * 51;
                hb_log( "encx264: encoding at constant QP %d",
                        param.rc.i_qp_constant );
                break;
        }
    }
    else if( job->vquality == 0 || job->vquality >= 1.0 )
    {
        /* Use the vquality as a raw RF or QP
          instead of treating it like a percentage. */
        switch( job->crf )
        {
            case 1:
                /*Constant RF*/
                param.rc.i_rc_method = X264_RC_CRF;
                param.rc.f_rf_constant = job->vquality;
                hb_log( "encx264: Encoding at constant RF %f",
                        param.rc.f_rf_constant );
                break;

            case 0:
                /*Constant QP*/
                param.rc.i_rc_method = X264_RC_CQP;
                param.rc.i_qp_constant = job->vquality;
                hb_log( "encx264: encoding at constant QP %d",
                        param.rc.i_qp_constant );
                break;
        }        
    }
    else
    {
        /* Rate control */
        param.rc.i_rc_method = X264_RC_ABR;
        param.rc.i_bitrate = job->vbitrate;
        switch( job->pass )
        {
            case 1:
                param.rc.b_stat_write  = 1;
                param.rc.psz_stat_out = pv->filename;
                break;
            case 2:
                param.rc.b_stat_read = 1;
                param.rc.psz_stat_in = pv->filename;
                break;
        }
    }

    hb_deep_log( 2, "encx264: opening libx264 (pass %d)", job->pass );
    pv->x264 = x264_encoder_open( &param );

    x264_encoder_headers( pv->x264, &nal, &nal_count );

    /* Sequence Parameter Set */
    x264_nal_encode( w->config->h264.sps, &nal_size, 0, &nal[1] );
    w->config->h264.sps_length = nal_size;

    /* Picture Parameter Set */
    x264_nal_encode( w->config->h264.pps, &nal_size, 0, &nal[2] );
    w->config->h264.pps_length = nal_size;

    x264_picture_alloc( &pv->pic_in, X264_CSP_I420,
                        job->width, job->height );

    pv->pic_in.img.i_stride[2] = pv->pic_in.img.i_stride[1] = ( ( job->width + 1 ) >> 1 );
    pv->x264_allocated_pic = pv->pic_in.img.plane[0];

    if (job->areBframes)
    {
        /* Basic initDelay value is the clockrate divided by the FPS
           -- the length of one frame in clockticks.                  */
        pv->init_delay = 90000. / ((double)job->vrate / (double)job->vrate_base);

        /* 23.976-length frames are 3753.75 ticks long on average but the DVD
           creates that average rate by repeating 59.95 fields so the max
           frame size is actually 4504.5 (3 field times). The field durations
           are computed based on quantized times (see below) so we need an extra
           two ticks to account for the rounding. */
        if (pv->init_delay == 3753)
            pv->init_delay = 4507;

        /* frame rates are not exact in the DVD 90KHz PTS clock (they are
           exact in the DVD 27MHz system clock but we never see that) so the
           rates computed above are all +-1 due to quantization. Worst case
           is when a clock-rounded-down frame is adjacent to a rounded-up frame
           which makes one of the frames 2 ticks longer than the nominal
           frame time. */
        pv->init_delay += 2;

        /* For VFR, libhb sees the FPS as 29.97, but the longest frames
           will use the duration of frames running at 23.976fps instead.. */
        if (job->vfr)
        {
            pv->init_delay = 7506;
        }

        /* The delay is 1 frames for regular b-frames, 2 for b-pyramid. */
        pv->init_delay *= job->areBframes;
    }
    w->config->h264.init_delay = pv->init_delay;

    return 0;
}

void encx264Close( hb_work_object_t * w )
{
    hb_work_private_t * pv = w->private_data;
    /*
     * Patch the x264 allocated data back in so that x264 can free it
     * we have been using our own buffers during the encode to avoid copying.
     */
    pv->pic_in.img.plane[0] = pv->x264_allocated_pic;
    x264_picture_clean( &pv->pic_in );
    x264_encoder_close( pv->x264 );
    free( pv );
    w->private_data = NULL;

    /* TODO */
}

/*
 * see comments in definition of 'frame_info' in pv struct for description
 * of what these routines are doing.
 */
static void save_frame_info( hb_work_private_t * pv, hb_buffer_t * in )
{
    int i = (in->start >> FRAME_INFO_MAX2) & FRAME_INFO_MASK;
    pv->frame_info[i].duration = in->stop - in->start;
}

static int64_t get_frame_duration( hb_work_private_t * pv, int64_t pts )
{
    int i = (pts >> FRAME_INFO_MAX2) & FRAME_INFO_MASK;
    return pv->frame_info[i].duration;
}

static hb_buffer_t *nal_encode( hb_work_object_t *w, x264_picture_t *pic_out,
                                int i_nal, x264_nal_t *nal )
{
    hb_buffer_t *buf = NULL;
    hb_work_private_t *pv = w->private_data;
    hb_job_t *job = pv->job;

    /* Should be way too large */
    buf = hb_video_buffer_init( job->width, job->height );
    buf->size = 0;
    buf->frametype = 0;

    // use the pts to get the original frame's duration.
    int64_t duration  = get_frame_duration( pv, pic_out->i_pts );
    buf->start = pic_out->i_pts;
    buf->stop  = pic_out->i_pts + duration;

    /* Encode all the NALs we were given into buf.
       NOTE: This code assumes one video frame per NAL (but there can
             be other stuff like SPS and/or PPS). If there are multiple
             frames we only get the duration of the first which will
             eventually screw up the muxer & decoder. */
    int i;
    for( i = 0; i < i_nal; i++ )
    {
        int data = buf->alloc - buf->size;
        int size = x264_nal_encode( buf->data + buf->size, &data, 1, &nal[i] );
        if( size < 1 )
        {
            continue;
        }

        if( job->mux & HB_MUX_AVI )
        {
            if( nal[i].i_ref_idc == NAL_PRIORITY_HIGHEST )
            {
                buf->frametype = HB_FRAME_KEY;
            }
            buf->size += size;
            continue;
        }

        /* H.264 in .mp4 or .mkv */
        int naltype = buf->data[buf->size+4] & 0x1f;
        if ( naltype == 0x7 || naltype == 0x8 )
        {
            // Sequence Parameter Set & Program Parameter Set go in the
            // mp4 header so skip them here
            continue;
        }

        /* H.264 in mp4 (stolen from mp4creator) */
        buf->data[buf->size+0] = ( ( size - 4 ) >> 24 ) & 0xFF;
        buf->data[buf->size+1] = ( ( size - 4 ) >> 16 ) & 0xFF;
        buf->data[buf->size+2] = ( ( size - 4 ) >>  8 ) & 0xFF;
        buf->data[buf->size+3] = ( ( size - 4 ) >>  0 ) & 0xFF;

        /* Decide what type of frame we have. */
        switch( pic_out->i_type )
        {
            case X264_TYPE_IDR:
                buf->frametype = HB_FRAME_IDR;
                /* if we have a chapter marker pending and this
                   frame's presentation time stamp is at or after
                   the marker's time stamp, use this as the
                   chapter start. */
                if( pv->next_chap != 0 && pv->next_chap <= pic_out->i_pts )
                {
                    pv->next_chap = 0;
                    buf->new_chap = pv->chap_mark;
                }
                break;

            case X264_TYPE_I:
                buf->frametype = HB_FRAME_I;
                break;

            case X264_TYPE_P:
                buf->frametype = HB_FRAME_P;
                break;

            case X264_TYPE_B:
                buf->frametype = HB_FRAME_B;
                break;

        /*  This is for b-pyramid, which has reference b-frames
            However, it doesn't seem to ever be used... */
            case X264_TYPE_BREF:
                buf->frametype = HB_FRAME_BREF;
                break;

            // If it isn't the above, what type of frame is it??
            default:
                buf->frametype = 0;
                break;
        }

        /* Since libx264 doesn't tell us when b-frames are
           themselves reference frames, figure it out on our own. */
        if( (buf->frametype == HB_FRAME_B) &&
            (nal[i].i_ref_idc != NAL_PRIORITY_DISPOSABLE) )
            buf->frametype = HB_FRAME_BREF;

        buf->size += size;
    }
    // make sure we found at least one video frame
    if ( buf->size <= 0 )
    {
        // no video - discard the buf
        hb_buffer_close( &buf );
    }
    return buf;
}

static hb_buffer_t *x264_encode( hb_work_object_t *w, hb_buffer_t *in )
{
    hb_work_private_t *pv = w->private_data;
    hb_job_t *job = pv->job;

    /* Point x264 at our current buffers Y(UV) data.  */
    pv->pic_in.img.plane[0] = in->data;

    int uvsize = ( (job->width + 1) >> 1 ) * ( (job->height + 1) >> 1 );
    if( job->grayscale )
    {
        /* XXX x264 has currently no option for grayscale encoding */
        memset( pv->pic_in.img.plane[1], 0x80, uvsize );
        memset( pv->pic_in.img.plane[2], 0x80, uvsize );
    }
    else
    {
        /* Point x264 at our buffers (Y)UV data */
        pv->pic_in.img.plane[1] = in->data + job->width * job->height;
        pv->pic_in.img.plane[2] = pv->pic_in.img.plane[1] + uvsize;
    }
    if( in->new_chap && job->chapter_markers )
    {
        /* chapters have to start with an IDR frame so request that this
           frame be coded as IDR. Since there may be up to 16 frames
           currently buffered in the encoder remember the timestamp so
           when this frame finally pops out of the encoder we'll mark
           its buffer as the start of a chapter. */
        pv->pic_in.i_type = X264_TYPE_IDR;
        if( pv->next_chap == 0 )
        {
            pv->next_chap = in->start;
            pv->chap_mark = in->new_chap;
        }
        /* don't let 'work_loop' put a chapter mark on the wrong buffer */
        in->new_chap = 0;
    }
    else
    {
        pv->pic_in.i_type = X264_TYPE_AUTO;
    }
    pv->pic_in.i_qpplus1 = 0;

    /* XXX this is temporary debugging code to check that the upstream
     * modules (render & sync) have generated a continuous, self-consistent
     * frame stream with the current frame's start time equal to the
     * previous frame's stop time.
     */
    if( pv->last_stop != in->start )
    {
        hb_log("encx264 input continuity err: last stop %lld  start %lld",
                pv->last_stop, in->start);
    }
    pv->last_stop = in->stop;

    // Remember info about this frame that we need to pass across
    // the x264_encoder_encode call (since it reorders frames).
    save_frame_info( pv, in );

    /* Feed the input PTS to x264 so it can figure out proper output PTS */
    pv->pic_in.i_pts = in->start;

    x264_picture_t pic_out;
    int i_nal;
    x264_nal_t *nal;

    x264_encoder_encode( pv->x264, &nal, &i_nal, &pv->pic_in, &pic_out );
    if ( i_nal > 0 )
    {
        return nal_encode( w, &pic_out, i_nal, nal );
    }
    return NULL;
}

int encx264Work( hb_work_object_t * w, hb_buffer_t ** buf_in,
                  hb_buffer_t ** buf_out )
{
    hb_work_private_t *pv = w->private_data;
    hb_buffer_t *in = *buf_in;

    *buf_out = NULL;

    if( in->size <= 0 )
    {
        // EOF on input. Flush any frames still in the decoder then
        // send the eof downstream to tell the muxer we're done.
        x264_picture_t pic_out;
        int i_nal;
        x264_nal_t *nal;
        hb_buffer_t *last_buf = NULL;

        while (1)
        {
            x264_encoder_encode( pv->x264, &nal, &i_nal, NULL, &pic_out );
            if ( i_nal <= 0 )
                break;

            hb_buffer_t *buf = nal_encode( w, &pic_out, i_nal, nal );
            if ( buf )
            {
                if ( last_buf == NULL )
                    *buf_out = buf;
                else
                    last_buf->next = buf;
                last_buf = buf;
            }
        }
        // Flushed everything - add the eof to the end of the chain.
        if ( last_buf == NULL )
            *buf_out = in;
        else
            last_buf->next = in;

        *buf_in = NULL;
        return HB_WORK_DONE;
    }

    // Not EOF - encode the packet & wrap it in a NAL

    // if we're re-ordering frames, check if this frame is too large to reorder
    if ( pv->init_delay && in->stop - in->start > pv->init_delay )
    {
        // This frame's duration is larger than the time allotted for b-frame
        // reordering. That means that if it's used as a reference the decoder
        // won't be able to move it early enough to render it in correct
        // sequence & the playback will have odd jumps & twitches. To make
        // sure this doesn't happen we pretend this frame is multiple
        // frames, each with duration <= init_delay. Since each of these
        // new frames contains the same image the visual effect is identical
        // to the original but the resulting stream can now be coded without
        // error. We take advantage of the fact that x264 buffers frame
        // data internally to feed the same image into the encoder multiple
        // times, just changing its start & stop times each time.
        int64_t orig_stop = in->stop;
        int64_t new_stop = in->start;
        hb_buffer_t *last_buf = NULL;

        // We want to spread the new frames uniformly over the total time
        // so that we don't end up with a very short frame at the end.
        // In the number of pieces calculation we add in init_delay-1 to
        // round up but not add an extra piece if the frame duration is
        // a multiple of init_delay. The final increment of frame_dur is
        // to restore the bits that got truncated by the divide on the
        // previous line. If we don't do this we end up with an extra tiny
        // frame at the end whose duration is npieces-1.
        int64_t frame_dur = orig_stop - new_stop;
        int64_t npieces = ( frame_dur + pv->init_delay - 1 ) / pv->init_delay;
        frame_dur /= npieces;
        ++frame_dur;

        while ( in->start < orig_stop )
        {
            new_stop += frame_dur;
            if ( new_stop > orig_stop )
                new_stop = orig_stop;
            in->stop = new_stop;
            hb_buffer_t *buf = x264_encode( w, in );
            if ( buf )
            {
                if ( last_buf == NULL )
                    *buf_out = buf;
                else
                    last_buf->next = buf;
                last_buf = buf;
            }
            in->start = new_stop;
        }
    }
    else
    {
        *buf_out = x264_encode( w, in );
    }
    return HB_WORK_OK;
}
