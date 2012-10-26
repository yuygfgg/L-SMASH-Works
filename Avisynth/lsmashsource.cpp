/*****************************************************************************
 * lsmashsource.cpp
 *****************************************************************************
 * Copyright (C) 2012 L-SMASH Works project
 *
 * Authors: Yusuke Nakamura <muken.the.vfrmaniac@gmail.com>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *****************************************************************************/

/* This file is available under an ISC license.
 * However, when distributing its binary file, it will be under LGPL or GPL.
 * Don't distribute it if its license is GPL. */

#ifndef __STDC_CONSTANT_MACROS
#define __STDC_CONSTANT_MACROS
#endif

#include <windows.h>
#include "avisynth.h"

extern "C"
{
/* L-SMASH */
#define LSMASH_DEMUXER_ENABLED
#include <lsmash.h>                 /* Demuxer */

/* Libav
 * The binary file will be LGPLed or GPLed. */
#include <libavformat/avformat.h>       /* Codec specific info importer */
#include <libavcodec/avcodec.h>         /* Decoder */
#include <libswscale/swscale.h>         /* Colorspace converter */
#include <libavresample/avresample.h>   /* Audio resampler */
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
}

#include "libavsmash.h"
#include "resample.h"

#pragma warning( disable:4996 )

#pragma comment( lib, "libgcc.a" )
#pragma comment( lib, "libz.a" )
#pragma comment( lib, "libbz2.a" )
#pragma comment( lib, "liblsmash.a" )
#pragma comment( lib, "libavutil.a" )
#pragma comment( lib, "libavcodec.a" )
#pragma comment( lib, "libavformat.a" )
#pragma comment( lib, "libswscale.a" )
#pragma comment( lib, "libavresample.a" )
#pragma comment( lib, "libwsock32.a" )

#ifndef INT32_MAX
#define INT32_MAX 0x7fffffffL
#endif

#define CLIP_VALUE( value, min, max ) ((value) > (max) ? (max) : (value) < (min) ? (min) : (value))

#define SEEK_MODE_NORMAL     0
#define SEEK_MODE_UNSAFE     1
#define SEEK_MODE_AGGRESSIVE 2

static void throw_error( void *message_priv, const char *message, ... )
{
    IScriptEnvironment *env = (IScriptEnvironment *)message_priv;
    char temp[256];
    va_list args;
    va_start( args, message );
    vsprintf( temp, message, args );
    va_end( args );
    env->ThrowError( (const char *)temp );
}

typedef struct
{
    uint32_t composition_to_decoding;
} order_converter_t;

typedef struct
{
    lsmash_root_t        *root;
    uint32_t              track_ID;
    uint32_t              forward_seek_threshold;
    int                   seek_mode;
    codec_configuration_t config;
    AVFormatContext      *format_ctx;
    AVFrame              *frame_buffer;
    struct SwsContext    *sws_ctx;
    order_converter_t    *order_converter;
    uint32_t              last_sample_number;
    uint32_t              last_rap_number;
} video_decode_handler_t;

typedef int func_make_frame( AVCodecContext *codec_ctx, struct SwsContext *sws_ctx, AVFrame *picture, PVideoFrame &frame, IScriptEnvironment *env );

class LSMASHVideoSource : public IClip
{
private:
    VideoInfo              vi;
    video_decode_handler_t vh;
    PVideoFrame           *first_valid_frame;
    uint32_t               first_valid_frame_number;
    func_make_frame       *make_frame;
    uint32_t open_file( const char *source, IScriptEnvironment *env );
    void get_video_track( const char *source, uint32_t track_number, int threads, IScriptEnvironment *env );
    void prepare_video_decoding( IScriptEnvironment *env );
public:
    LSMASHVideoSource( const char *source, uint32_t track_number, int threads, int seek_mode, uint32_t forward_seek_threshold, IScriptEnvironment *env );
    ~LSMASHVideoSource();
    PVideoFrame __stdcall GetFrame( int n, IScriptEnvironment *env );
    bool __stdcall GetParity( int n ) { return false; }
    void __stdcall GetAudio( void *buf, __int64 start, __int64 count, IScriptEnvironment *env ) {}
    void __stdcall SetCacheHints( int cachehints, int frame_range ) {}
    const VideoInfo& __stdcall GetVideoInfo() { return vi; }
};

LSMASHVideoSource::LSMASHVideoSource( const char *source, uint32_t track_number, int threads, int seek_mode, uint32_t forward_seek_threshold, IScriptEnvironment *env )
{
    memset( &vi, 0, sizeof(VideoInfo) );
    memset( &vh, 0, sizeof(video_decode_handler_t) );
    vh.seek_mode              = seek_mode;
    vh.forward_seek_threshold = forward_seek_threshold;
    first_valid_frame         = NULL;
    get_video_track( source, track_number, threads, env );
    lsmash_discard_boxes( vh.root );
    prepare_video_decoding( env );
}

LSMASHVideoSource::~LSMASHVideoSource()
{
    if( first_valid_frame )
        delete first_valid_frame;
    if( vh.order_converter )
        delete [] vh.order_converter;
    if( vh.frame_buffer )
        avcodec_free_frame( &vh.frame_buffer );
    if( vh.sws_ctx )
        sws_freeContext( vh.sws_ctx );
    cleanup_configuration( &vh.config );
    if( vh.format_ctx )
        avformat_close_input( &vh.format_ctx );
    lsmash_destroy_root( vh.root );
}

uint32_t LSMASHVideoSource::open_file( const char *source, IScriptEnvironment *env )
{
    /* L-SMASH */
    vh.root = lsmash_open_movie( source, LSMASH_FILE_MODE_READ );
    if( !vh.root )
        env->ThrowError( "LSMASHVideoSource: failed to lsmash_open_movie." );
    lsmash_movie_parameters_t movie_param;
    lsmash_initialize_movie_parameters( &movie_param );
    lsmash_get_movie_parameters( vh.root, &movie_param );
    if( movie_param.number_of_tracks == 0 )
        env->ThrowError( "LSMASHVideoSource: the number of tracks equals 0." );
    /* libavformat */
    av_register_all();
    avcodec_register_all();
    if( avformat_open_input( &vh.format_ctx, source, NULL, NULL ) )
        env->ThrowError( "LSMASHVideoSource: failed to avformat_open_input." );
    if( avformat_find_stream_info( vh.format_ctx, NULL ) < 0 )
        env->ThrowError( "LSMASHVideoSource: failed to avformat_find_stream_info." );
    /* */
    vh.config.error_message = throw_error;
    return movie_param.number_of_tracks;
}

static inline uint64_t get_gcd( uint64_t a, uint64_t b )
{
    if( !b )
        return a;
    while( 1 )
    {
        uint64_t c = a % b;
        if( !c )
            return b;
        a = b;
        b = c;
    }
}

static inline uint64_t reduce_fraction( uint64_t *a, uint64_t *b )
{
    uint64_t reduce = get_gcd( *a, *b );
    *a /= reduce;
    *b /= reduce;
    return reduce;
}

static void setup_timestamp_info( video_decode_handler_t *hp, VideoInfo *vi, uint64_t media_timescale, IScriptEnvironment *env )
{
    if( vi->num_frames == 1 )
    {
        /* Calculate average framerate. */
        uint64_t media_duration = lsmash_get_media_duration_from_media_timeline( hp->root, hp->track_ID );
        if( media_duration == 0 )
            media_duration = INT32_MAX;
        reduce_fraction( &media_timescale, &media_duration );
        vi->fps_numerator   = (unsigned int)media_timescale;
        vi->fps_denominator = (unsigned int)media_duration;
        return;
    }
    lsmash_media_ts_list_t ts_list;
    if( lsmash_get_media_timestamps( hp->root, hp->track_ID, &ts_list ) )
        env->ThrowError( "LSMASHVideoSource: failed to get timestamps." );
    if( ts_list.sample_count != vi->num_frames )
        env->ThrowError( "LSMASHVideoSource: failed to count number of video samples." );
    uint32_t composition_sample_delay;
    if( lsmash_get_max_sample_delay( &ts_list, &composition_sample_delay ) )
    {
        lsmash_delete_media_timestamps( &ts_list );
        env->ThrowError( "LSMASHVideoSource: failed to get composition delay." );
    }
    if( composition_sample_delay )
    {
        /* Consider composition order for keyframe detection.
         * Note: sample number for L-SMASH is 1-origin. */
        hp->order_converter = new order_converter_t[ts_list.sample_count + 1];
        if( !hp->order_converter )
        {
            lsmash_delete_media_timestamps( &ts_list );
            env->ThrowError( "LSMASHVideoSource: failed to allocate memory." );
        }
        for( uint32_t i = 0; i < ts_list.sample_count; i++ )
            ts_list.timestamp[i].dts = i + 1;
        lsmash_sort_timestamps_composition_order( &ts_list );
        for( uint32_t i = 0; i < ts_list.sample_count; i++ )
            hp->order_converter[i + 1].composition_to_decoding = (uint32_t)ts_list.timestamp[i].dts;
    }
    /* Calculate average framerate. */
    uint64_t largest_cts          = ts_list.timestamp[1].cts;
    uint64_t second_largest_cts   = ts_list.timestamp[0].cts;
    uint64_t composition_timebase = ts_list.timestamp[1].cts - ts_list.timestamp[0].cts;
    for( uint32_t i = 2; i < ts_list.sample_count; i++ )
    {
        if( ts_list.timestamp[i].cts == ts_list.timestamp[i - 1].cts )
        {
            lsmash_delete_media_timestamps( &ts_list );
            return;
        }
        composition_timebase = get_gcd( composition_timebase, ts_list.timestamp[i].cts - ts_list.timestamp[i - 1].cts );
        second_largest_cts = largest_cts;
        largest_cts = ts_list.timestamp[i].cts;
    }
    uint64_t reduce = reduce_fraction( &media_timescale, &composition_timebase );
    uint64_t composition_duration = ((largest_cts - ts_list.timestamp[0].cts) + (largest_cts - second_largest_cts)) / reduce;
    lsmash_delete_media_timestamps( &ts_list );
    vi->fps_numerator   = (unsigned int)((vi->num_frames * ((double)media_timescale / composition_duration)) * composition_timebase + 0.5);
    vi->fps_denominator = (unsigned int)composition_timebase;
}

void LSMASHVideoSource::get_video_track( const char *source, uint32_t track_number, int threads, IScriptEnvironment *env )
{
    uint32_t number_of_tracks = open_file( source, env );
    if( track_number && track_number > number_of_tracks )
        env->ThrowError( "LSMASHVideoSource: the number of tracks equals %I32u.", number_of_tracks );
    /* L-SMASH */
    uint32_t i;
    lsmash_media_parameters_t media_param;
    if( track_number == 0 )
    {
        /* Get the first video track. */
        for( i = 1; i <= number_of_tracks; i++ )
        {
            vh.track_ID = lsmash_get_track_ID( vh.root, i );
            if( vh.track_ID == 0 )
                env->ThrowError( "LSMASHVideoSource: failed to find video track." );
            lsmash_initialize_media_parameters( &media_param );
            if( lsmash_get_media_parameters( vh.root, vh.track_ID, &media_param ) )
                env->ThrowError( "LSMASHVideoSource: failed to get media parameters." );
            if( media_param.handler_type == ISOM_MEDIA_HANDLER_TYPE_VIDEO_TRACK )
                break;
        }
        if( i > number_of_tracks )
            env->ThrowError( "LSMASHVideoSource: failed to find video track." );
    }
    else
    {
        /* Get the desired video track. */
        vh.track_ID = lsmash_get_track_ID( vh.root, track_number );
        if( vh.track_ID == 0 )
            env->ThrowError( "LSMASHVideoSource: failed to find video track." );
        lsmash_initialize_media_parameters( &media_param );
        if( lsmash_get_media_parameters( vh.root, vh.track_ID, &media_param ) )
            env->ThrowError( "LSMASHVideoSource: failed to get media parameters." );
        if( media_param.handler_type != ISOM_MEDIA_HANDLER_TYPE_VIDEO_TRACK )
            env->ThrowError( "LSMASHVideoSource: the track you specified is not a video track." );
    }
    if( lsmash_construct_timeline( vh.root, vh.track_ID ) )
        env->ThrowError( "LSMASHVideoSource: failed to get construct timeline." );
    if( get_summaries( vh.root, vh.track_ID, &vh.config ) )
        env->ThrowError( "LSMASHVideoSource: failed to get summaries." );
    vi.num_frames = lsmash_get_sample_count_in_media_timeline( vh.root, vh.track_ID );
    setup_timestamp_info( &vh, &vi, media_param.timescale, env );
    /* libavformat */
    for( i = 0; i < vh.format_ctx->nb_streams && vh.format_ctx->streams[i]->codec->codec_type != AVMEDIA_TYPE_VIDEO; i++ );
    if( i == vh.format_ctx->nb_streams )
        env->ThrowError( "LSMASHVideoSource: failed to find stream by libavformat." );
    /* libavcodec */
    AVStream *stream = vh.format_ctx->streams[i];
    AVCodecContext *ctx = stream->codec;
    vh.config.ctx = ctx;
    AVCodec *codec = avcodec_find_decoder( ctx->codec_id );
    if( !codec )
        env->ThrowError( "LSMASHVideoSource: failed to find %s decoder.", codec->name );
    ctx->thread_count = threads;
    if( avcodec_open2( ctx, codec, NULL ) < 0 )
        env->ThrowError( "LSMASHVideoSource: failed to avcodec_open2." );
}

static int get_conversion_multiplier( enum AVPixelFormat dst_pix_fmt, enum AVPixelFormat src_pix_fmt, int width )
{
    int src_size = 0;
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get( src_pix_fmt );
    int used_plane[4] = { 0, 0, 0, 0 };
    for( int i = 0; i < desc->nb_components; i++ )
    {
        int plane = desc->comp[i].plane;
        if( used_plane[plane] )
            continue;
        src_size += av_image_get_linesize( src_pix_fmt, width, plane );
        used_plane[plane] = 1;
    }
    if( src_size == 0 )
        return 1;
    int dst_size = 0;
    desc = av_pix_fmt_desc_get( dst_pix_fmt );
    used_plane[0] = used_plane[1] = used_plane[2] = used_plane[3] = 0;
    for( int i = 0; i < desc->nb_components; i++ )
    {
        int plane = desc->comp[i].plane;
        if( used_plane[plane] )
            continue;
        dst_size += av_image_get_linesize( dst_pix_fmt, width, plane );
        used_plane[plane] = 1;
    }
    return (dst_size - 1) / src_size + 1;
}

static int make_frame_yuv420p( AVCodecContext *codec_ctx, struct SwsContext *sws_ctx, AVFrame *picture, PVideoFrame &frame, IScriptEnvironment *env )
{
    int abs_dst_linesize = picture->linesize[0] > 0 ? picture->linesize[0] : -picture->linesize[0];
    if( abs_dst_linesize & 15 )
        abs_dst_linesize = (abs_dst_linesize & 0xfffffff0) + 16;  /* Make mod16. */
    uint8_t *dst_data[4];
    dst_data[0] = (uint8_t *)av_mallocz( abs_dst_linesize * codec_ctx->height * 3 );
    if( !dst_data[0] )
        return -1;
    for( int i = 1; i < 3; i++ )
        dst_data[i] = dst_data[i - 1] + abs_dst_linesize * codec_ctx->height;
    dst_data[3] = NULL;
    const int dst_linesize[4] = { abs_dst_linesize, abs_dst_linesize, abs_dst_linesize, 0 };
    sws_scale( sws_ctx, (const uint8_t* const*)picture->data, picture->linesize, 0, codec_ctx->height, dst_data, dst_linesize );
    env->BitBlt( frame->GetWritePtr( PLANAR_Y ), frame->GetPitch( PLANAR_Y ), dst_data[0], dst_linesize[0], frame->GetRowSize( PLANAR_Y ), frame->GetHeight( PLANAR_Y ) ); 
    env->BitBlt( frame->GetWritePtr( PLANAR_U ), frame->GetPitch( PLANAR_U ), dst_data[1], dst_linesize[1], frame->GetRowSize( PLANAR_U ), frame->GetHeight( PLANAR_U ) ); 
    env->BitBlt( frame->GetWritePtr( PLANAR_V ), frame->GetPitch( PLANAR_V ), dst_data[2], dst_linesize[2], frame->GetRowSize( PLANAR_V ), frame->GetHeight( PLANAR_V ) ); 
    av_free( dst_data[0] );
    return 0;
}

static int make_frame_yuv422( AVCodecContext *codec_ctx, struct SwsContext *sws_ctx, AVFrame *picture, PVideoFrame &frame, IScriptEnvironment *env )
{
    int abs_dst_linesize = picture->linesize[0] + picture->linesize[1] + picture->linesize[2] + picture->linesize[3];
    if( abs_dst_linesize < 0 )
        abs_dst_linesize = -abs_dst_linesize;
    const int dst_linesize[4] = { abs_dst_linesize, 0, 0, 0 };
    uint8_t  *dst_data    [4] = { NULL, NULL, NULL, NULL };
    dst_data[0] = (uint8_t *)av_mallocz( dst_linesize[0] * codec_ctx->height );
    if( !dst_data[0] )
        return -1;
    sws_scale( sws_ctx, (const uint8_t* const*)picture->data, picture->linesize, 0, codec_ctx->height, dst_data, dst_linesize );
    env->BitBlt( frame->GetWritePtr(), frame->GetPitch(), dst_data[0], dst_linesize[0], frame->GetRowSize(), frame->GetHeight() );
    av_free( dst_data[0] );
    return 0;
}

static int make_frame_rgba32( AVCodecContext *codec_ctx, struct SwsContext *sws_ctx, AVFrame *picture, PVideoFrame &frame, IScriptEnvironment *env )
{
    int abs_dst_linesize = picture->linesize[0] + picture->linesize[1] + picture->linesize[2] + picture->linesize[3];
    if( abs_dst_linesize < 0 )
        abs_dst_linesize = -abs_dst_linesize;
    abs_dst_linesize *= get_conversion_multiplier( AV_PIX_FMT_BGRA, codec_ctx->pix_fmt, codec_ctx->width );
    const int dst_linesize[4] = { abs_dst_linesize, 0, 0, 0 };
    uint8_t  *dst_data    [4] = { NULL, NULL, NULL, NULL };
    dst_data[0] = (uint8_t *)av_mallocz( dst_linesize[0] * codec_ctx->height );
    if( !dst_data[0] )
        return -1;
    sws_scale( sws_ctx, (const uint8_t* const*)picture->data, picture->linesize, 0, codec_ctx->height, dst_data, dst_linesize );
    env->BitBlt( frame->GetWritePtr() + frame->GetPitch() * (frame->GetHeight() - 1), -frame->GetPitch(), dst_data[0], dst_linesize[0], frame->GetRowSize(), frame->GetHeight() ); 
    av_free( dst_data[0] );
    return 0;
}

static void avoid_yuv_scale_conversion( enum AVPixelFormat *input_pixel_format )
{
    static const struct
    {
        enum AVPixelFormat full;
        enum AVPixelFormat limited;
    } range_hack_table[]
        = {
            { AV_PIX_FMT_YUVJ420P, AV_PIX_FMT_YUV420P },
            { AV_PIX_FMT_YUVJ422P, AV_PIX_FMT_YUV422P },
            { AV_PIX_FMT_NONE,     AV_PIX_FMT_NONE    }
          };
    for( int i = 0; range_hack_table[i].full != AV_PIX_FMT_NONE; i++ )
        if( *input_pixel_format == range_hack_table[i].full )
            *input_pixel_format = range_hack_table[i].limited;
}

func_make_frame *determine_colorspace_conversion( enum AVPixelFormat *input_pixel_format, enum AVPixelFormat *output_pixel_format, int *output_pixel_type )
{
    avoid_yuv_scale_conversion( input_pixel_format );
    switch( *input_pixel_format )
    {
        case AV_PIX_FMT_YUV420P :
        case AV_PIX_FMT_NV12 :
        case AV_PIX_FMT_NV21 :
            *output_pixel_format = AV_PIX_FMT_YUV420P;  /* planar YUV 4:2:0, 12bpp, (1 Cr & Cb sample per 2x2 Y samples) */
            *output_pixel_type   = VideoInfo::CS_I420;
            return make_frame_yuv420p;
        case AV_PIX_FMT_YUYV422 :
        case AV_PIX_FMT_YUV422P :
        case AV_PIX_FMT_UYVY422 :
            *output_pixel_format = AV_PIX_FMT_YUYV422;  /* packed YUV 4:2:2, 16bpp */
            *output_pixel_type   = VideoInfo::CS_YUY2;
            return make_frame_yuv422;
        case AV_PIX_FMT_ARGB :
        case AV_PIX_FMT_RGBA :
        case AV_PIX_FMT_ABGR :
        case AV_PIX_FMT_BGRA :
        case AV_PIX_FMT_RGB24 :
        case AV_PIX_FMT_BGR24 :
        case AV_PIX_FMT_GBRP :
            *output_pixel_format = AV_PIX_FMT_BGRA;     /* packed BGRA 8:8:8:8, 32bpp, BGRABGRA... */
            *output_pixel_type   = VideoInfo::CS_BGR32;
            return make_frame_rgba32;
        default :
            *output_pixel_format = AV_PIX_FMT_NONE;
            *output_pixel_type   = VideoInfo::CS_UNKNOWN;
            return NULL;
    }
}

void LSMASHVideoSource::prepare_video_decoding( IScriptEnvironment *env )
{
    vh.frame_buffer = avcodec_alloc_frame();
    if( !vh.frame_buffer )
        env->ThrowError( "LSMASHVideoSource: failed to allocate video frame buffer." );
    /* Initialize the video decoder configuration. */
    codec_configuration_t *config = &vh.config;
    config->message_priv = env;
    if( initialize_decoder_configuration( vh.root, vh.track_ID, config ) )
        env->ThrowError( "LSMASHVideoSource: failed to initialize the decoder configuration." );
    /* swscale */
    enum AVPixelFormat input_pixel_format = config->ctx->pix_fmt;
    enum AVPixelFormat output_pixel_format;
    make_frame = determine_colorspace_conversion( &config->ctx->pix_fmt, &output_pixel_format, &vi.pixel_type );
    if( !make_frame )
        env->ThrowError( "LSMASHVideoSource: %s is not supported", av_get_pix_fmt_name( input_pixel_format ) );
    vi.width  = config->ctx->width;
    vi.height = config->ctx->height;
    vh.sws_ctx = sws_getCachedContext( NULL,
                                       config->ctx->width, config->ctx->height, config->ctx->pix_fmt,
                                       config->ctx->width, config->ctx->height, output_pixel_format,
                                       SWS_FAST_BILINEAR, NULL, NULL, NULL );
    if( !vh.sws_ctx )
        env->ThrowError( "LSMASHVideoSource: failed to get swscale context." );
    /* Find the first valid video sample. */
    for( uint32_t i = 1; i <= vi.num_frames + get_decoder_delay( config->ctx ); i++ )
    {
        AVPacket pkt = { 0 };
        get_sample( vh.root, vh.track_ID, i, &vh.config, &pkt );
        AVFrame *picture = vh.frame_buffer;
        avcodec_get_frame_defaults( picture );
        int got_picture;
        if( avcodec_decode_video2( config->ctx, picture, &got_picture, &pkt ) >= 0 && got_picture )
        {
            first_valid_frame_number = i - min( get_decoder_delay( config->ctx ), config->delay_count );
            if( first_valid_frame_number > 1 || vi.num_frames == 1 )
            {
                PVideoFrame temp = env->NewVideoFrame( vi );
                if( !temp )
                    env->ThrowError( "LSMASHVideoSource: failed to allocate memory for the first valid video frame data." );
                if( make_frame( config->ctx, vh.sws_ctx, picture, temp, env ) )
                    continue;
                first_valid_frame = new PVideoFrame( temp );
            }
            break;
        }
        else if( pkt.data )
            ++ config->delay_count;
    }
    vh.last_sample_number = vi.num_frames + 1;  /* Force seeking at the first reading. */
}

static int decode_video_sample( video_decode_handler_t *hp, AVFrame *picture, int *got_picture, uint32_t sample_number )
{
    AVPacket pkt = { 0 };
    int ret = get_sample( hp->root, hp->track_ID, sample_number, &hp->config, &pkt );
    if( ret )
        return ret;
    if( pkt.flags != ISOM_SAMPLE_RANDOM_ACCESS_TYPE_NONE )
    {
        pkt.flags = AV_PKT_FLAG_KEY;
        hp->last_rap_number = sample_number;
    }
    else
        pkt.flags = 0;
    avcodec_get_frame_defaults( picture );
    uint64_t cts = pkt.pts;
    ret = avcodec_decode_video2( hp->config.ctx, picture, got_picture, &pkt );
    picture->pts = cts;
    return ret < 0 ? -1 : 0;
}

static inline uint32_t get_decoding_sample_number( order_converter_t *order_converter, uint32_t composition_sample_number )
{
    return order_converter
         ? order_converter[composition_sample_number].composition_to_decoding
         : composition_sample_number;
}

static int find_random_accessible_point( video_decode_handler_t *hp, uint32_t composition_sample_number, uint32_t decoding_sample_number, uint32_t *rap_number )
{
    if( decoding_sample_number == 0 )
        decoding_sample_number = get_decoding_sample_number( hp->order_converter, composition_sample_number );
    lsmash_random_access_type rap_type;
    uint32_t distance;  /* distance from the closest random accessible point to the previous. */
    uint32_t number_of_leadings;
    if( lsmash_get_closest_random_accessible_point_detail_from_media_timeline( hp->root, hp->track_ID, decoding_sample_number,
                                                                               rap_number, &rap_type, &number_of_leadings, &distance ) )
        *rap_number = 1;
    int roll_recovery = (rap_type == ISOM_SAMPLE_RANDOM_ACCESS_TYPE_POST_ROLL || rap_type == ISOM_SAMPLE_RANDOM_ACCESS_TYPE_PRE_ROLL);
    int is_leading    = number_of_leadings && (decoding_sample_number - *rap_number <= number_of_leadings);
    if( (roll_recovery || is_leading) && *rap_number > distance )
        *rap_number -= distance;
    /* Check whether random accessible point has the same decoder configuration or not. */
    decoding_sample_number = get_decoding_sample_number( hp->order_converter, composition_sample_number );
    do
    {
        lsmash_sample_t sample;
        lsmash_sample_t rap_sample;
        if( lsmash_get_sample_info_from_media_timeline( hp->root, hp->track_ID, decoding_sample_number, &sample )
         || lsmash_get_sample_info_from_media_timeline( hp->root, hp->track_ID, *rap_number, &rap_sample ) )
        {
            /* Fatal error. */
            *rap_number = hp->last_rap_number;
            return 0;
        }
        if( sample.index == rap_sample.index )
            break;
        uint32_t sample_index = sample.index;
        for( uint32_t i = decoding_sample_number - 1; i; i-- )
        {
            if( lsmash_get_sample_info_from_media_timeline( hp->root, hp->track_ID, i, &sample ) )
            {
                /* Fatal error. */
                *rap_number = hp->last_rap_number;
                return 0;
            }
            if( sample.index != sample_index )
            {
                if( distance )
                {
                    *rap_number += distance;
                    distance = 0;
                    continue;
                }
                else
                    *rap_number = i + 1;
            }
        }
        break;
    } while( 1 );
    return roll_recovery;
}

static uint32_t seek_video( video_decode_handler_t *hp, AVFrame *picture, uint32_t composition_sample_number, uint32_t rap_number, int error_ignorance )
{
    /* Prepare to decode from random accessible sample. */
    codec_configuration_t *config = &hp->config;
    if( config->update_pending )
        /* Update the decoder configuration. */
        update_configuration( hp->root, hp->track_ID, config );
    else
        flush_buffers( config );
    if( config->error )
        return 0;
    int dummy;
    uint64_t rap_cts = 0;
    uint32_t i;
    uint32_t decoder_delay = get_decoder_delay( config->ctx );
    for( i = rap_number; i < composition_sample_number + decoder_delay; i++ )
    {
        if( config->index == config->queue.index )
            config->delay_count = min( decoder_delay, i - rap_number );
        int ret = decode_video_sample( hp, picture, &dummy, i );
        /* Some decoders return -1 when feeding a leading sample.
         * We don't consider as an error if the return value -1 is caused by a leading sample since it's not fatal at all. */
        if( i == hp->last_rap_number )
            rap_cts = picture->pts;
        if( ret == -1 && (uint64_t)picture->pts >= rap_cts && !error_ignorance )
            return 0;
        else if( ret >= 1 )
            /* No decoding occurs. */
            break;
    }
    if( config->index == config->queue.index )
        config->delay_count = min( decoder_delay, i - rap_number );
    return i;
}

static int get_picture( video_decode_handler_t *hp, AVFrame *picture, uint32_t current, uint32_t goal, uint32_t sample_count )
{
    codec_configuration_t *config = &hp->config;
    int got_picture = (current > goal);
    while( current <= goal )
    {
        int ret = decode_video_sample( hp, picture, &got_picture, current );
        if( ret == -1 )
            return -1;
        else if( ret == 1 )
            /* Sample doesn't exist. */
            break;
        ++current;
        if( config->update_pending )
            /* A new decoder configuration is needed. Anyway, stop getting picture. */
            break;
        if( !got_picture )
            ++ config->delay_count;
    }
    /* Flush the last frames. */
    if( current > sample_count && get_decoder_delay( config->ctx ) )
        while( current <= goal )
        {
            AVPacket pkt = { 0 };
            av_init_packet( &pkt );
            pkt.data = NULL;
            pkt.size = 0;
            avcodec_get_frame_defaults( picture );
            if( avcodec_decode_video2( config->ctx, picture, &got_picture, &pkt ) < 0 )
                return -1;
            ++current;
        }
    return got_picture ? 0 : -1;
}

PVideoFrame __stdcall LSMASHVideoSource::GetFrame( int n, IScriptEnvironment *env )
{
#define MAX_ERROR_COUNT 3       /* arbitrary */
    uint32_t sample_number = n + 1;     /* For L-SMASH, sample_number is 1-origin. */
    if( sample_number < first_valid_frame_number || vi.num_frames == 1 )
    {
        /* Copy the first valid video frame. */
        vh.last_sample_number = vi.num_frames + 1;  /* Force seeking at the next access for valid video sample. */
        return *first_valid_frame;
    }
    codec_configuration_t *config = &vh.config;
    config->message_priv = env;
    PVideoFrame frame = env->NewVideoFrame( vi );
    if( config->error )
        return frame;
    AVFrame *picture = vh.frame_buffer;
    uint32_t start_number;  /* number of sample, for normal decoding, where decoding starts excluding decoding delay */
    uint32_t rap_number;    /* number of sample, for seeking, where decoding starts excluding decoding delay */
    int seek_mode = vh.seek_mode;
    int roll_recovery = 0;
    if( sample_number > vh.last_sample_number
     && sample_number <= vh.last_sample_number + vh.forward_seek_threshold )
    {
        start_number = vh.last_sample_number + 1 + config->delay_count;
        rap_number = vh.last_rap_number;
    }
    else
    {
        roll_recovery = find_random_accessible_point( &vh, sample_number, 0, &rap_number );
        if( rap_number == vh.last_rap_number && sample_number > vh.last_sample_number )
        {
            roll_recovery = 0;
            start_number = vh.last_sample_number + 1 + config->delay_count;
        }
        else
        {
            /* Require starting to decode from random accessible sample. */
            vh.last_rap_number = rap_number;
            start_number = seek_video( &vh, picture, sample_number, rap_number, roll_recovery || seek_mode != SEEK_MODE_NORMAL );
        }
    }
    /* Get desired picture. */
    int error_count = 0;
    while( start_number == 0    /* Failed to seek. */
     || config->update_pending  /* Need to update the decoder configuration to decode pictures. */
     || get_picture( &vh, picture, start_number, sample_number + config->delay_count, vi.num_frames ) )
    {
        if( config->update_pending )
        {
            roll_recovery = find_random_accessible_point( &vh, sample_number, 0, &rap_number );
            vh.last_rap_number = rap_number;
        }
        else
        {
            /* Failed to get desired picture. */
            if( config->error || seek_mode == SEEK_MODE_AGGRESSIVE )
                env->ThrowError( "LSMASHVideoSource: fatal error of decoding." );
            if( ++error_count > MAX_ERROR_COUNT || rap_number <= 1 )
            {
                if( seek_mode == SEEK_MODE_UNSAFE )
                    env->ThrowError( "LSMASHVideoSource: fatal error of decoding." );
                /* Retry to decode from the same random accessible sample with error ignorance. */
                seek_mode = SEEK_MODE_AGGRESSIVE;
            }
            else
            {
                /* Retry to decode from more past random accessible sample. */
                roll_recovery = find_random_accessible_point( &vh, sample_number, rap_number - 1, &rap_number );
                if( vh.last_rap_number == rap_number )
                    env->ThrowError( "LSMASHVideoSource: fatal error of decoding." );
                vh.last_rap_number = rap_number;
            }
        }
        start_number = seek_video( &vh, picture, sample_number, rap_number, roll_recovery || seek_mode != SEEK_MODE_NORMAL );
    }
    vh.last_sample_number = sample_number;
    if( make_frame( config->ctx, vh.sws_ctx, picture, frame, env ) )
        env->ThrowError( "LSMASHVideoSource: failed to make a frame." );
    return frame;
#undef MAX_ERROR_COUNT
}

typedef struct
{
    lsmash_root_t          *root;
    uint32_t                track_ID;
    codec_configuration_t   config;
    AVFormatContext        *format_ctx;
    AVFrame                *frame_buffer;
    AVAudioResampleContext *avr_ctx;
    uint8_t                *resampled_buffer;
    int                     resampled_buffer_size;
    AVPacket                packet;
    enum AVSampleFormat     output_sample_format;
    uint32_t                frame_count;
    uint32_t                last_frame_number;
    uint64_t                output_channel_layout;
    uint64_t                next_pcm_sample_number;
    uint64_t                skip_samples;
    int                     implicit_preroll;
    int                     planes;
    int                     input_block_align;
    int                     output_block_align;
    int                     output_sample_rate;
    int                     output_bits_per_sample;
    int                     s24_output;
} audio_decode_handler_t;

class LSMASHAudioSource : public IClip
{
private:
    VideoInfo              vi;
    audio_decode_handler_t ah;
    uint32_t open_file( const char *source, IScriptEnvironment *env );
    void get_audio_track( const char *source, uint32_t track_number, bool skip_priming, IScriptEnvironment *env );
    void prepare_audio_decoding( IScriptEnvironment *env );
public:
    LSMASHAudioSource( const char *source, uint32_t track_number, bool skip_priming, IScriptEnvironment *env );
    ~LSMASHAudioSource();
    PVideoFrame __stdcall GetFrame( int n, IScriptEnvironment *env ) { return NULL; }
    bool __stdcall GetParity( int n ) { return false; }
    void __stdcall GetAudio( void *buf, __int64 start, __int64 wanted_length, IScriptEnvironment *env );
    void __stdcall SetCacheHints( int cachehints, int frame_range ) {}
    const VideoInfo& __stdcall GetVideoInfo() { return vi; }
};

LSMASHAudioSource::LSMASHAudioSource( const char *source, uint32_t track_number, bool skip_priming, IScriptEnvironment *env )
{
    memset( &vi, 0, sizeof(VideoInfo) );
    memset( &ah, 0, sizeof(audio_decode_handler_t) );
    get_audio_track( source, track_number, skip_priming, env );
    lsmash_discard_boxes( ah.root );
    prepare_audio_decoding( env );
}

LSMASHAudioSource::~LSMASHAudioSource()
{
    if( ah.resampled_buffer )
        av_free( ah.resampled_buffer );
    if( ah.frame_buffer )
        avcodec_free_frame( &ah.frame_buffer );
    if( ah.avr_ctx )
        avresample_free( &ah.avr_ctx );
    cleanup_configuration( &ah.config );
    if( ah.format_ctx )
        avformat_close_input( &ah.format_ctx );
    lsmash_destroy_root( ah.root );
}

uint32_t LSMASHAudioSource::open_file( const char *source, IScriptEnvironment *env )
{
    /* L-SMASH */
    ah.root = lsmash_open_movie( source, LSMASH_FILE_MODE_READ );
    if( !ah.root )
        env->ThrowError( "LSMASHAudioSource: failed to lsmash_open_movie." );
    lsmash_movie_parameters_t movie_param;
    lsmash_initialize_movie_parameters( &movie_param );
    lsmash_get_movie_parameters( ah.root, &movie_param );
    if( movie_param.number_of_tracks == 0 )
        env->ThrowError( "LSMASHAudioSource: the number of tracks equals 0." );
    /* libavformat */
    av_register_all();
    avcodec_register_all();
    if( avformat_open_input( &ah.format_ctx, source, NULL, NULL ) )
        env->ThrowError( "LSMASHAudioSource: failed to avformat_open_input." );
    if( avformat_find_stream_info( ah.format_ctx, NULL ) < 0 )
        env->ThrowError( "LSMASHAudioSource: failed to avformat_find_stream_info." );
    /* */
    ah.config.error_message = throw_error;
    return movie_param.number_of_tracks;
}

static int64_t get_start_time( lsmash_root_t *root, uint32_t track_ID )
{
    /* Consider start time of this media if any non-empty edit is present. */
    uint32_t edit_count = lsmash_count_explicit_timeline_map( root, track_ID );
    for( uint32_t edit_number = 1; edit_number <= edit_count; edit_number++ )
    {
        lsmash_edit_t edit;
        if( lsmash_get_explicit_timeline_map( root, track_ID, edit_number, &edit ) )
            return 0;
        if( edit.duration == 0 )
            return 0;   /* no edits */
        if( edit.start_time >= 0 )
            return edit.start_time;
    }
    return 0;
}

static char *duplicate_as_string( void *src, size_t length )
{
    char *dst = new char[length + 1];
    if( !dst )
        return NULL;
    memcpy( dst, src, length );
    dst[length] = '\0';
    return dst;
}

void LSMASHAudioSource::get_audio_track( const char *source, uint32_t track_number, bool skip_priming, IScriptEnvironment *env )
{
    uint32_t number_of_tracks = open_file( source, env );
    if( track_number && track_number > number_of_tracks )
        env->ThrowError( "LSMASHAudioSource: the number of tracks equals %I32u.", number_of_tracks );
    /* L-SMASH */
    uint32_t i;
    lsmash_media_parameters_t media_param;
    if( track_number == 0 )
    {
        /* Get the first audio track. */
        for( i = 1; i <= number_of_tracks; i++ )
        {
            ah.track_ID = lsmash_get_track_ID( ah.root, i );
            if( ah.track_ID == 0 )
                env->ThrowError( "LSMASHAudioSource: failed to find audio track." );
            lsmash_initialize_media_parameters( &media_param );
            if( lsmash_get_media_parameters( ah.root, ah.track_ID, &media_param ) )
                env->ThrowError( "LSMASHAudioSource: failed to get media parameters." );
            if( media_param.handler_type == ISOM_MEDIA_HANDLER_TYPE_AUDIO_TRACK )
                break;
        }
        if( i > number_of_tracks )
            env->ThrowError( "LSMASHAudioSource: failed to find audio track." );
    }
    else
    {
        /* Get the desired audio track. */
        ah.track_ID = lsmash_get_track_ID( ah.root, track_number );
        if( ah.track_ID == 0 )
            env->ThrowError( "LSMASHAudioSource: failed to find audio track." );
        lsmash_initialize_media_parameters( &media_param );
        if( lsmash_get_media_parameters( ah.root, ah.track_ID, &media_param ) )
            env->ThrowError( "LSMASHAudioSource: failed to get media parameters." );
        if( media_param.handler_type != ISOM_MEDIA_HANDLER_TYPE_AUDIO_TRACK )
            env->ThrowError( "LSMASHAudioSource: the track you specified is not an audio track." );
    }
    if( lsmash_construct_timeline( ah.root, ah.track_ID ) )
        env->ThrowError( "LSMASHAudioSource: failed to get construct timeline." );
    if( get_summaries( ah.root, ah.track_ID, &ah.config ) )
        env->ThrowError( "LSMASHAudioSource: failed to get summaries." );
    ah.frame_count = lsmash_get_sample_count_in_media_timeline( ah.root, ah.track_ID );
    vi.num_audio_samples = lsmash_get_media_duration_from_media_timeline( ah.root, ah.track_ID );
    if( skip_priming )
    {
        uint32_t itunes_metadata_count = lsmash_count_itunes_metadata( ah.root );
        for( i = 1; i <= itunes_metadata_count; i++ )
        {
            lsmash_itunes_metadata_t metadata;
            if( lsmash_get_itunes_metadata( ah.root, i, &metadata ) )
                continue;
            if( metadata.item != ITUNES_METADATA_ITEM_CUSTOM
             || (metadata.type != ITUNES_METADATA_TYPE_STRING && metadata.type != ITUNES_METADATA_TYPE_BINARY)
             || !metadata.meaning || !metadata.name
             || memcmp( "com.apple.iTunes", metadata.meaning, strlen( metadata.meaning ) )
             || memcmp( "iTunSMPB", metadata.name, strlen( metadata.name ) ) )
                continue;
            char *value;
            if( metadata.type == ITUNES_METADATA_TYPE_STRING )
            {
                int length = strlen( metadata.value.string );
                if( length < 116 )
                    continue;
                value = duplicate_as_string( metadata.value.string, length );
            }
            else    /* metadata.type == ITUNES_METADATA_TYPE_BINARY */
            {
                if( metadata.value.binary.size < 116 )
                    continue;
                value = duplicate_as_string( metadata.value.binary.data, metadata.value.binary.size );
            }
            if( !value )
                continue;
            uint32_t dummy[9];
            uint32_t priming_samples;
            uint32_t padding;
            uint64_t duration;
            if( 12 != sscanf( value, " %I32x %I32x %I32x %I64x %I32x %I32x %I32x %I32x %I32x %I32x %I32x %I32x",
                              &dummy[0], &priming_samples, &padding, &duration, &dummy[1], &dummy[2],
                              &dummy[3], &dummy[4], &dummy[5], &dummy[6], &dummy[7], &dummy[8] ) )
            {
                delete [] value;
                continue;
            }
            delete [] value;
            ah.implicit_preroll  = 1;
            ah.skip_samples      = priming_samples;
            vi.num_audio_samples = duration + priming_samples;
            break;
        }
        if( ah.skip_samples == 0 )
        {
            uint32_t ctd_shift;
            if( lsmash_get_composition_to_decode_shift_from_media_timeline( ah.root, ah.track_ID, &ctd_shift ) )
                env->ThrowError( "LSMASHAudioSource: failed to get the timeline shift." );
            ah.skip_samples = ctd_shift + get_start_time( ah.root, ah.track_ID );
        }
    }
    /* libavformat */
    for( i = 0; i < ah.format_ctx->nb_streams && ah.format_ctx->streams[i]->codec->codec_type != AVMEDIA_TYPE_AUDIO; i++ );
    if( i == ah.format_ctx->nb_streams )
        env->ThrowError( "LSMASHAudioSource: failed to find stream by libavformat." );
    /* libavcodec */
    AVStream *stream = ah.format_ctx->streams[i];
    AVCodecContext *ctx = stream->codec;
    ah.config.ctx = ctx;
    AVCodec *codec = avcodec_find_decoder( ctx->codec_id );
    if( !codec )
        env->ThrowError( "LSMASHAudioSource: failed to find %s decoder.", codec->name );
    ctx->thread_count = 0;
    if( avcodec_open2( ctx, codec, NULL ) < 0 )
        env->ThrowError( "LSMASHAudioSource: failed to avcodec_open2." );
}

static uint64_t count_overall_pcm_samples( audio_decode_handler_t *hp )
{
    codec_configuration_t *config = &hp->config;
    libavsmash_summary_t  *s      = NULL;
    int      current_sample_rate          = 0;
    uint32_t current_index                = 0;
    uint32_t current_frame_length         = 0;
    uint32_t audio_frame_count            = 0;
    uint64_t pcm_sample_count             = 0;
    uint64_t overall_pcm_sample_count     = 0;
    uint64_t skip_samples                 = 0;
    uint64_t prior_sequences_sample_count = 0;
    for( uint32_t i = 1; i <= hp->frame_count; i++ )
    {
        /* Get configuration index. */
        lsmash_sample_t sample;
        if( lsmash_get_sample_info_from_media_timeline( hp->root, hp->track_ID, i, &sample ) )
            continue;
        if( current_index != sample.index )
        {
            s = &config->entries[ sample.index - 1 ];
            current_index = sample.index;
        }
        else if( !s )
            continue;
        /* Get audio frame length. */
        uint32_t frame_length;
        if( s->extended.frame_length )
            frame_length = s->extended.frame_length;
        else if( lsmash_get_sample_delta_from_media_timeline( hp->root, hp->track_ID, i, &frame_length ) )
            continue;
        /* */
        if( (current_sample_rate != s->extended.sample_rate && s->extended.sample_rate > 0)
         || current_frame_length != frame_length )
        {
            if( current_sample_rate > 0 )
            {
                if( hp->skip_samples > pcm_sample_count )
                    skip_samples += pcm_sample_count * s->extended.upsampling;
                else if( hp->skip_samples > prior_sequences_sample_count )
                    skip_samples += (hp->skip_samples - prior_sequences_sample_count) * s->extended.upsampling;
                prior_sequences_sample_count += pcm_sample_count;
                pcm_sample_count *= s->extended.upsampling;
                uint64_t resampled_sample_count = hp->output_sample_rate == current_sample_rate || pcm_sample_count == 0
                                                ? pcm_sample_count
                                                : (pcm_sample_count * hp->output_sample_rate - 1) / current_sample_rate + 1;
                overall_pcm_sample_count += resampled_sample_count;
                audio_frame_count = 0;
                pcm_sample_count  = 0;
            }
            current_sample_rate  = s->extended.sample_rate > 0 ? s->extended.sample_rate : config->ctx->sample_rate;
            current_frame_length = frame_length;
        }
        pcm_sample_count += frame_length;
        ++audio_frame_count;
    }
    if( !s || (pcm_sample_count == 0 && overall_pcm_sample_count == 0) )
        return 0;
    if( hp->skip_samples > prior_sequences_sample_count )
        skip_samples += (hp->skip_samples - prior_sequences_sample_count) * s->extended.upsampling;
    pcm_sample_count *= s->extended.upsampling;
    current_sample_rate = s->extended.sample_rate > 0 ? s->extended.sample_rate : config->ctx->sample_rate;
    if( current_sample_rate == hp->output_sample_rate )
    {
        hp->skip_samples = skip_samples;
        if( pcm_sample_count )
            overall_pcm_sample_count += pcm_sample_count;
    }
    else
    {
        if( skip_samples )
            hp->skip_samples = ((uint64_t)skip_samples * hp->output_sample_rate - 1) / current_sample_rate + 1;
        if( pcm_sample_count )
            overall_pcm_sample_count += (pcm_sample_count * hp->output_sample_rate - 1) / current_sample_rate + 1;
    }
    return overall_pcm_sample_count - hp->skip_samples;
}

static inline enum AVSampleFormat decide_audio_output_sample_format( enum AVSampleFormat input_sample_format )
{
    /* Avisynth doesn't support IEEE double precision floating point format. */
    switch( input_sample_format )
    {
        case AV_SAMPLE_FMT_U8 :
        case AV_SAMPLE_FMT_U8P :
            return AV_SAMPLE_FMT_U8;
        case AV_SAMPLE_FMT_S16 :
        case AV_SAMPLE_FMT_S16P :
            return AV_SAMPLE_FMT_S16;
        case AV_SAMPLE_FMT_S32 :
        case AV_SAMPLE_FMT_S32P :
            return AV_SAMPLE_FMT_S32;
        default :
            return AV_SAMPLE_FMT_FLT;
    }
}

void LSMASHAudioSource::prepare_audio_decoding( IScriptEnvironment *env )
{
    ah.frame_buffer = avcodec_alloc_frame();
    if( !ah.frame_buffer )
        env->ThrowError( "LSMASHAudioSource: failed to allocate audio frame buffer." );
    /* Initialize the audio decoder configuration. */
    codec_configuration_t *config = &ah.config;
    config->message_priv = env;
    if( initialize_decoder_configuration( ah.root, ah.track_ID, config ) )
        env->ThrowError( "LSMASHAudioSource: failed to initialize the decoder configuration." );
    ah.output_channel_layout  = config->prefer.channel_layout;
    ah.output_sample_format   = config->prefer.sample_format;
    ah.output_sample_rate     = config->prefer.sample_rate;
    ah.output_bits_per_sample = config->prefer.bits_per_sample;
    /* */
    vi.num_audio_samples = count_overall_pcm_samples( &ah );
    if( vi.num_audio_samples == 0 )
        env->ThrowError( "LSMASHAudioSource: no valid audio frame." );
    ah.next_pcm_sample_number = vi.num_audio_samples + 1;   /* Force seeking at the first reading. */
    /* Set up resampler. */
    ah.avr_ctx = avresample_alloc_context();
    if( !ah.avr_ctx )
        env->ThrowError( "LSMASHAudioSource: failed to avresample_alloc_context." );
    if( config->ctx->channel_layout == 0 )
        config->ctx->channel_layout = av_get_default_channel_layout( config->ctx->channels );
    ah.output_sample_format = decide_audio_output_sample_format( ah.output_sample_format );
    av_opt_set_int( ah.avr_ctx, "in_channel_layout",   config->ctx->channel_layout, 0 );
    av_opt_set_int( ah.avr_ctx, "in_sample_fmt",       config->ctx->sample_fmt,     0 );
    av_opt_set_int( ah.avr_ctx, "in_sample_rate",      config->ctx->sample_rate,    0 );
    av_opt_set_int( ah.avr_ctx, "out_channel_layout",  ah.output_channel_layout,    0 );
    av_opt_set_int( ah.avr_ctx, "out_sample_fmt",      ah.output_sample_format,     0 );
    av_opt_set_int( ah.avr_ctx, "out_sample_rate",     ah.output_sample_rate,       0 );
    av_opt_set_int( ah.avr_ctx, "internal_sample_fmt", AV_SAMPLE_FMT_FLTP,          0 );
    if( avresample_open( ah.avr_ctx ) < 0 )
        env->ThrowError( "LSMASHAudioSource: failed to open resampler." );
    /* Decide output Bits Per Sample. */
    int output_channels = av_get_channel_layout_nb_channels( ah.output_channel_layout );
    if( ah.output_sample_format == AV_SAMPLE_FMT_S32
     && (ah.output_bits_per_sample == 0 || ah.output_bits_per_sample == 24) )
    {
        /* 24bit signed integer output */
        if( config->ctx->frame_size )
        {
            ah.resampled_buffer_size = get_linesize( output_channels, config->ctx->frame_size, ah.output_sample_format );
            ah.resampled_buffer      = (uint8_t *)av_malloc( ah.resampled_buffer_size );
            if( !ah.resampled_buffer )
                env->ThrowError( "LSMASHAudioSource: failed to allocate memory for resampling." );
        }
        ah.s24_output             = 1;
        ah.output_bits_per_sample = 24;
    }
    else
        ah.output_bits_per_sample = av_get_bytes_per_sample( ah.output_sample_format ) * 8;
    /* */
    vi.nchannels                = output_channels;
    vi.audio_samples_per_second = ah.output_sample_rate;
    switch ( ah.output_sample_format )
    {
        case AV_SAMPLE_FMT_U8 :
        case AV_SAMPLE_FMT_U8P :
            vi.sample_type = SAMPLE_INT8;
            break;
        case AV_SAMPLE_FMT_S16 :
        case AV_SAMPLE_FMT_S16P :
            vi.sample_type = SAMPLE_INT16;
            break;
        case AV_SAMPLE_FMT_S32 :
        case AV_SAMPLE_FMT_S32P :
            vi.sample_type = ah.s24_output ? SAMPLE_INT24 : SAMPLE_INT32;
            break;
        case AV_SAMPLE_FMT_FLT :
        case AV_SAMPLE_FMT_FLTP :
            vi.sample_type = SAMPLE_FLOAT;
            break;
        default :
            env->ThrowError( "LSMASHAudioSource: %s is not supported.", av_get_sample_fmt_name( config->ctx->sample_fmt ) );
    }
    /* Set up the number of planes and the block alignment of decoded and output data. */
    int input_channels = av_get_channel_layout_nb_channels( config->ctx->channel_layout );
    if( av_sample_fmt_is_planar( config->ctx->sample_fmt ) )
    {
        ah.planes            = input_channels;
        ah.input_block_align = av_get_bytes_per_sample( config->ctx->sample_fmt );
    }
    else
    {
        ah.planes            = 1;
        ah.input_block_align = av_get_bytes_per_sample( config->ctx->sample_fmt ) * input_channels;
    }
    ah.output_block_align = (output_channels * ah.output_bits_per_sample) / 8;
}

static inline int get_frame_length( audio_decode_handler_t *hp, uint32_t frame_number, uint32_t *frame_length, libavsmash_summary_t **sp )
{
    lsmash_sample_t sample;
    if( lsmash_get_sample_info_from_media_timeline( hp->root, hp->track_ID, frame_number, &sample ) )
        return -1;
    *sp = &hp->config.entries[ sample.index - 1 ];
    libavsmash_summary_t *s = *sp;
    if( s->extended.frame_length == 0 )
    {
        /* variable frame length
         * Guess the frame length from sample duration. */
        if( lsmash_get_sample_delta_from_media_timeline( hp->root, hp->track_ID, frame_number, frame_length ) )
            return -1;
        *frame_length *= s->extended.upsampling;
    }
    else
        /* constant frame length */
        *frame_length = s->extended.frame_length;
    return 0;
}

static uint32_t get_preroll_samples( audio_decode_handler_t *hp, uint32_t *frame_number )
{
    /* Some audio CODEC requires pre-roll for correct composition. */
    lsmash_sample_property_t prop;
    if( lsmash_get_sample_property_from_media_timeline( hp->root, hp->track_ID, *frame_number, &prop ) )
        return 0;
    if( prop.pre_roll.distance == 0 )
    {
        if( hp->skip_samples == 0 || !hp->implicit_preroll )
            return 0;
        /* Estimate pre-roll distance. */
        uint64_t skip_samples = hp->skip_samples;
        for( uint32_t i = 1; i <= hp->frame_count || skip_samples; i++ )
        {
            libavsmash_summary_t *dummy = NULL;
            uint32_t frame_length;
            if( get_frame_length( hp, i, &frame_length, &dummy ) )
                break;
            if( skip_samples < frame_length )
                skip_samples = 0;
            else
                skip_samples -= frame_length;
            ++ prop.pre_roll.distance;
        }
    }
    uint32_t preroll_samples = 0;
    for( uint32_t i = 0; i < prop.pre_roll.distance; i++ )
    {
        if( *frame_number > 1 )
            --(*frame_number);
        else
            break;
        libavsmash_summary_t *dummy = NULL;
        uint32_t frame_length;
        if( get_frame_length( hp, *frame_number, &frame_length, &dummy ) )
            break;
        preroll_samples += frame_length;
    }
    return preroll_samples;
}

static int find_start_audio_frame( audio_decode_handler_t *hp, uint64_t start_frame_pos, uint64_t *start_offset )
{
    uint32_t frame_number                    = 1;
    uint64_t current_frame_pos               = 0;
    uint64_t next_frame_pos                  = 0;
    int      current_sample_rate             = 0;
    uint32_t current_frame_length            = 0;
    uint64_t pcm_sample_count                = 0;   /* the number of accumulated PCM samples before resampling per sequence */
    uint64_t resampled_sample_count          = 0;   /* the number of accumulated PCM samples after resampling per sequence */
    uint64_t prior_sequences_resampled_count = 0;   /* the number of accumulated PCM samples of all prior sequences */
    do
    {
        current_frame_pos = next_frame_pos;
        libavsmash_summary_t *s = NULL;
        uint32_t frame_length;
        if( get_frame_length( hp, frame_number, &frame_length, &s ) )
        {
            ++frame_number;
            continue;
        }
        if( (current_sample_rate != s->extended.sample_rate && s->extended.sample_rate > 0)
         || current_frame_length != frame_length )
        {
            /* Encountered a new sequence. */
            prior_sequences_resampled_count += resampled_sample_count;
            pcm_sample_count = 0;
            current_sample_rate  = s->extended.sample_rate > 0 ? s->extended.sample_rate : hp->config.ctx->sample_rate;
            current_frame_length = frame_length;
        }
        pcm_sample_count += frame_length;
        resampled_sample_count = hp->output_sample_rate == current_sample_rate || pcm_sample_count == 0
                               ? pcm_sample_count
                               : (pcm_sample_count * hp->output_sample_rate - 1) / current_sample_rate + 1;
        next_frame_pos = prior_sequences_resampled_count + resampled_sample_count;
        if( start_frame_pos < next_frame_pos )
            break;
        ++frame_number;
    } while( frame_number <= hp->frame_count );
    *start_offset = start_frame_pos - current_frame_pos;
    if( *start_offset && current_sample_rate != hp->output_sample_rate )
        *start_offset = (*start_offset * current_sample_rate - 1) / hp->output_sample_rate + 1;
    *start_offset += get_preroll_samples( hp, &frame_number );
    return frame_number;
}

static int waste_decoded_audio_samples( audio_decode_handler_t *hp, int input_sample_count, int wanted_sample_count, uint8_t **out_data, int sample_offset )
{
    /* Input */
    uint8_t **in_data = new uint8_t *[ hp->planes ];
    if( !in_data )
        return 0;
    int decoded_data_offset = sample_offset * hp->input_block_align;
    for( int i = 0; i < hp->planes; i++ )
        in_data[i] = hp->frame_buffer->extended_data[i] + decoded_data_offset;
    audio_samples_t in;
    in.channel_layout = hp->frame_buffer->channel_layout;
    in.sample_count   = input_sample_count;
    in.sample_format  = (enum AVSampleFormat)hp->frame_buffer->format;
    in.data           = in_data;
    /* Output */
    uint8_t *resampled_buffer = NULL;
    if( hp->s24_output )
    {
        int out_channels = get_channel_layout_nb_channels( hp->output_channel_layout );
        int out_linesize = get_linesize( out_channels, wanted_sample_count, hp->output_sample_format );
        if( !hp->resampled_buffer || out_linesize > hp->resampled_buffer_size )
        {
            uint8_t *temp = (uint8_t *)av_realloc( hp->resampled_buffer, out_linesize );
            if( !temp )
            {
                delete [] in_data;
                return 0;
            }
            hp->resampled_buffer_size = out_linesize;
            hp->resampled_buffer      = temp;
        }
        resampled_buffer = hp->resampled_buffer;
    }
    audio_samples_t out;
    out.channel_layout = hp->output_channel_layout;
    out.sample_count   = wanted_sample_count;
    out.sample_format  = hp->output_sample_format;
    out.data           = resampled_buffer ? &resampled_buffer : out_data;
    /* Resample */
    int resampled_size = resample_audio( hp->avr_ctx, &out, &in );
    if( resampled_buffer && resampled_size > 0 )
        resampled_size = resample_s32_to_s24( out_data, hp->resampled_buffer, resampled_size );
    delete [] in_data;
    return resampled_size > 0 ? resampled_size / hp->output_block_align : 0;
}

void __stdcall LSMASHAudioSource::GetAudio( void *buf, __int64 start, __int64 wanted_length, IScriptEnvironment *env )
{
    codec_configuration_t *config = &ah.config;
    if( config->error )
        return;
    uint32_t frame_number;
    uint64_t seek_offset;
    uint64_t output_length = 0;
    if( start > 0 && start == ah.next_pcm_sample_number )
    {
        frame_number = ah.last_frame_number;
        if( ah.frame_buffer->extended_data[0] )
        {
            /* Flush remaing audio samples. */
            int resampled_length = waste_decoded_audio_samples( &ah, 0, (int)wanted_length, (uint8_t **)&buf, 0 );
            output_length += resampled_length;
            wanted_length -= resampled_length;
            if( wanted_length <= 0 )
                goto audio_out;
        }
        if( ah.packet.size <= 0 )
            ++frame_number;
        seek_offset = 0;
    }
    else
    {
        /* Seek audio stream. */
        if( flush_resampler_buffers( ah.avr_ctx ) < 0 )
        {
            config->error = 1;
            return;
        }
        flush_buffers( config );
        if( config->error )
            return;
        ah.next_pcm_sample_number = 0;
        ah.last_frame_number      = 0;
        uint64_t start_frame_pos;
        if( start >= 0 )
            start_frame_pos = start;
        else
        {
            uint64_t silence_length = -start;
            put_silence_audio_samples( (int)(silence_length * ah.output_block_align), (uint8_t **)&buf );
            output_length += silence_length;
            wanted_length -= silence_length;
            start_frame_pos = 0;
        }
        start_frame_pos += ah.skip_samples;
        frame_number = find_start_audio_frame( &ah, start_frame_pos, &seek_offset );
    }
    do
    {
        AVPacket *pkt = &ah.packet;
        if( frame_number > ah.frame_count )
        {
            if( config->delay_count )
            {
                /* Null packet */
                av_init_packet( pkt );
                pkt->data = NULL;
                pkt->size = 0;
                -- config->delay_count;
            }
            else
                goto audio_out;
        }
        else if( pkt->size <= 0 )
            /* Getting a sample must be after flushing all remaining samples in resampler's FIFO buffer. */
            while( get_sample( ah.root, ah.track_ID, frame_number, config, pkt ) == 2 )
                if( config->update_pending )
                    /* Update the decoder configuration. */
                    update_configuration( ah.root, ah.track_ID, config );
        int output_audio = 0;
        do
        {
            avcodec_get_frame_defaults( ah.frame_buffer );
            int decode_complete;
            int wasted_data_length = avcodec_decode_audio4( config->ctx, ah.frame_buffer, &decode_complete, pkt );
            if( wasted_data_length < 0 )
            {
                pkt->size = 0;  /* Force to get the next sample. */
                break;
            }
            if( pkt->data )
            {
                pkt->size -= wasted_data_length;
                pkt->data += wasted_data_length;
            }
            else if( !decode_complete )
                goto audio_out;
            if( decode_complete && ah.frame_buffer->extended_data[0] )
            {
                /* Check channel layout, sample rate and sample format of decoded audio samples. */
                int64_t channel_layout;
                int64_t sample_rate;
                int64_t sample_format;
                av_opt_get_int( ah.avr_ctx, "in_channel_layout", 0, &channel_layout );
                av_opt_get_int( ah.avr_ctx, "in_sample_rate",    0, &sample_rate );
                av_opt_get_int( ah.avr_ctx, "in_sample_fmt",     0, &sample_format );
                if( ah.frame_buffer->channel_layout == 0 )
                    ah.frame_buffer->channel_layout = av_get_default_channel_layout( config->ctx->channels );
                if( ah.frame_buffer->channel_layout != (uint64_t)channel_layout
                 || ah.frame_buffer->sample_rate    != (int)sample_rate
                 || ah.frame_buffer->format         != (enum AVSampleFormat)sample_format )
                {
                    /* Detected a change of channel layout, sample rate or sample format.
                     * Reconfigure audio resampler. */
                    if( update_resampler_configuration( ah.avr_ctx,
                                                        ah.output_channel_layout,
                                                        ah.output_sample_rate,
                                                        ah.output_sample_format,
                                                        ah.frame_buffer->channel_layout,
                                                        ah.frame_buffer->sample_rate,
                                                        (enum AVSampleFormat)ah.frame_buffer->format,
                                                        &ah.planes,
                                                        &ah.input_block_align ) < 0 )
                    {
                        config->error = 1;
                        goto audio_out;
                    }
                }
                /* Process decoded audio samples. */
                int decoded_length = ah.frame_buffer->nb_samples;
                if( decoded_length > seek_offset )
                {
                    /* Send decoded audio data to resampler and get desired resampled audio as you want as much as possible. */
                    int useful_length = (int)(decoded_length - seek_offset);
                    int resampled_length = waste_decoded_audio_samples( &ah, useful_length, (int)wanted_length, (uint8_t **)&buf, (int)seek_offset );
                    output_length += resampled_length;
                    wanted_length -= resampled_length;
                    seek_offset = 0;
                    if( wanted_length <= 0 )
                        goto audio_out;
                }
                else
                    seek_offset -= decoded_length;
                output_audio = 1;
            }
        } while( pkt->size > 0 );
        if( !output_audio && pkt->data )    /* Count audio frame delay only if feeding non-NULL packet. */
            ++ config->delay_count;
        ++frame_number;
    } while( 1 );
audio_out:
    ah.next_pcm_sample_number = start + output_length;
    ah.last_frame_number = frame_number;
}

AVSValue __cdecl CreateLSMASHVideoSource( AVSValue args, void *user_data, IScriptEnvironment *env )
{
    const char *source                 = args[0].AsString();
    uint32_t    track_number           = args[1].AsInt( 0 );
    int         threads                = args[2].AsInt( 0 );
    int         seek_mode              = args[3].AsInt( 0 );
    uint32_t    forward_seek_threshold = args[4].AsInt( 10 );
    threads                = threads >= 0 ? threads : 0;
    seek_mode              = CLIP_VALUE( seek_mode, 0, 2 );
    forward_seek_threshold = CLIP_VALUE( forward_seek_threshold, 1, 999 );
    return new LSMASHVideoSource( source, track_number, threads, seek_mode, forward_seek_threshold, env );
}

AVSValue __cdecl CreateLSMASHAudioSource( AVSValue args, void *user_data, IScriptEnvironment *env )
{
    const char *source       = args[0].AsString();
    uint32_t    track_number = args[1].AsInt( 0 );
    bool        skip_priming = args[2].AsBool( true );
    return new LSMASHAudioSource( source, track_number, skip_priming, env );
}

extern "C" __declspec(dllexport) const char * __stdcall AvisynthPluginInit2( IScriptEnvironment *env )
{
    env->AddFunction( "LSMASHVideoSource", "[source]s[track]i[threads]i[seek_mode]i[seek_threshold]i", CreateLSMASHVideoSource, 0 );
    env->AddFunction( "LSMASHAudioSource", "[source]s[track]i[skip_priming]b", CreateLSMASHAudioSource, 0 );
    return "LSMASHSource";
}
