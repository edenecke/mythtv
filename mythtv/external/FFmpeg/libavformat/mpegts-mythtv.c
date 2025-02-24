/*
 * MPEG2 transport stream (aka DVB) demuxer
 * Copyright (c) 2002-2003 Fabrice Bellard
 * Reworked for use with MythTV
 *
 * This file is part of MythTV.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "libavutil/crc.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/log.h"
#include "libavutil/dict.h"
#include "libavutil/mathematics.h"
#include "libavutil/opt.h"
#include "libavutil/avassert.h"
#include "libavcodec/bytestream.h"
#include "libavcodec/avcodec.h"
#include "libavcodec/get_bits.h"
#include "avformat.h"
#include "mpegts-mythtv.h"
#include "internal.h"
#include "avio_internal.h"
#include "mpeg.h"
#include "isom.h"
#include "internal.h"

/**
 * av_dlog macros
 * copied from libavutil/log.h since they are deprecated and removed from there
 * Useful to print debug messages that shouldn't get compiled in normally.
 */

#ifdef DEBUG
#    define av_dlog(pctx, ...) av_log(pctx, AV_LOG_DEBUG, __VA_ARGS__)
#else
#    define av_dlog(pctx, ...) do { if (0) av_log(pctx, AV_LOG_DEBUG, __VA_ARGS__); } while (0)
#endif

/* maximum size in which we look for synchronisation if
   synchronisation is lost */
#define MAX_RESYNC_SIZE 65536

#define MAX_PES_PAYLOAD 200*1024

#define MAX_MP4_DESCR_COUNT 16

#define PMT_NOT_YET_FOUND 0
#define PMT_NOT_IN_PAT    1
#define PMT_FOUND         2

typedef struct SectionContext SectionContext;

static SectionContext *add_section_stream(MpegTSContext *ts, int pid, int stream_type);
static void mpegts_cleanup_streams(MpegTSContext *ts);
static int find_in_list(const int *pids, int pid);

enum MpegTSFilterType {
    MPEGTS_PES,
    MPEGTS_SECTION,
};

typedef struct MpegTSFilter MpegTSFilter;


static int is_pat_same(MpegTSContext *mpegts_ctx,
                       int *pmt_pnums, int *pmts_pids, unsigned int pmt_count);

static void mpegts_add_stream(MpegTSContext *ts, int id, pmt_entry_t* item, uint32_t prog_reg_desc, int pcr_pid);
static int pmt_equal_streams(MpegTSContext *mpegts_ctx,
                             pmt_entry_t* items, int item_cnt);

typedef int PESCallback(MpegTSFilter *f, const uint8_t *buf, int len, int is_start, int64_t pos);

typedef struct MpegTSPESFilter {
    PESCallback *pes_cb;
    void *opaque;
} MpegTSPESFilter;

typedef void SectionCallback(MpegTSFilter *f, const uint8_t *buf, int len);

typedef void SetServiceCallback(void *opaque, int ret);

typedef struct MpegTSSectionFilter {
    int section_index;
    int section_h_size;
    uint8_t *section_buf;
    unsigned int check_crc:1;
    unsigned int end_of_section_reached:1;
    SectionCallback *section_cb;
    void *opaque;
} MpegTSSectionFilter;

struct MpegTSFilter {
    int pid;
    int es_id;
    int last_cc; /* last cc code (-1 if first packet) */
    enum MpegTSFilterType type;
    /** if set, chop off PMT at the end of the TS packet, regardless of the
     *  data length given in the packet.  This is for use by BBC iPlayer IPTV
     *  recordings which seem to only want to send the first packet of the PMT
     *  but give a length that requires 3 packets.  Without this, those
     *  recordings are unplayable */
    int pmt_chop_at_ts;
    union {
        MpegTSPESFilter pes_filter;
        MpegTSSectionFilter section_filter;
    } u;
};

/** maximum number of PMT's we expect to be described in a PAT */
#define PAT_MAX_PMT 128

/** maximum number of streams we expect to be described in a PMT */
#define PMT_PIDS_MAX 256

#define MAX_PIDS_PER_PROGRAM 64
struct Program {
    unsigned int id; //program id/service id
    unsigned int pid; // PMT PID
    unsigned int nb_pids;
    unsigned int pids[MAX_PIDS_PER_PROGRAM];
};

struct MpegTSContext {
    const AVClass *class;
    /* user data */
    AVFormatContext *stream;
    /** raw packet size, including FEC if present            */
    int raw_packet_size;

    int pos47;

    /** if true, all pids are analyzed to find streams       */
    int auto_guess;

    /** compute exact PCR for each transport stream packet   */
    int mpeg2ts_compute_pcr;

    int64_t cur_pcr;    /**< used to estimate the exact PCR  */
    int pcr_incr;       /**< used to estimate the exact PCR  */

    /** if set, stop_parse is set when PAT/PMT is found      */
    int scanning;
    /* data needed to handle file based ts */
    /** stop parsing loop                                    */
    int stop_parse;
    /** set to PMT_NOT_IN_PAT in pat_cb when scanning is true
     *  and the MPEG program number "req_sid" is not found in PAT;
     *  set to PMT_FOUND when a PMT with a the "req_sid" program
     *  number is found. */
    int pmt_scan_state;
    /** packet containing Audio/Video data                   */
    AVPacket *pkt;
    /** to detect seek                                       */
    int64_t last_pos;

    /******************************************/
    /* private mpegts data */
    /* scan context */
    /** structure to keep track of Program->pids mapping     */
    unsigned int nb_prg;
    struct Program *prg;

    /** filter for the PAT                                   */
    MpegTSFilter *pat_filter;
    /** filter for the PMT for the MPEG program number specified by req_sid */
    MpegTSFilter *pmt_filter;
    /** MPEG program number of stream we want to decode      */
    int req_sid;

    /** filters for various streams specified by PMT + for the PAT and PMT */
    MpegTSFilter *pids[NB_PID_MAX];

    /** number of streams in the last PMT seen */
    int pid_cnt;
    /** list of streams in the last PMT seen */
    int pmt_pids[PMT_PIDS_MAX];
};

static const AVOption options[] = {
    {"compute_pcr", "Compute exact PCR for each transport stream packet.", offsetof(MpegTSContext, mpeg2ts_compute_pcr), AV_OPT_TYPE_INT,
     {.dbl = 0}, 0, 1, AV_OPT_FLAG_DECODING_PARAM },
    { NULL },
};

static const AVClass mpegtsraw_class = {
    .class_name = "mpegtsraw demuxer",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

/* TS stream handling */

enum MpegTSState {
    MPEGTS_HEADER = 0,
    MPEGTS_PESHEADER,
    MPEGTS_PESHEADER_FILL,
    MPEGTS_PAYLOAD,
    MPEGTS_SKIP,
};

/* enough for PES header + length */
#define PES_START_SIZE  6
#define PES_HEADER_SIZE 9
#define MAX_PES_HEADER_SIZE (9 + 255)

typedef struct PESContext {
    int pid;
    int pcr_pid; /**< if -1 then all packets containing PCR are considered */
    int stream_type;
    MpegTSContext *ts;
    AVFormatContext *stream;
    AVStream *st;
    AVStream *sub_st; /**< stream for the embedded AC3 stream in HDMV TrueHD */
    enum MpegTSState state;
    /* used to get the format */
    int data_index;
    int flags; /**< copied to the AVPacket flags */
    int total_size;
    int pes_header_size;
    int extended_stream_id;
    int64_t pts, dts;
    int64_t ts_packet_pos; /**< position of first TS packet of this PES packet */
    uint8_t header[MAX_PES_HEADER_SIZE];
    uint8_t *buffer;
    SLConfigDescr sl;
} PESContext;

extern AVInputFormat ff_mythtv_mpegts_demuxer;

struct SectionContext {
    int pid;
    int stream_type;
    int new_packet;
    MpegTSContext *ts;
    AVFormatContext *stream;
    AVStream *st;
};

static void clear_program(MpegTSContext *ts, unsigned int programid)
{
    int i;

    for(i=0; i<ts->nb_prg; i++)
        if(ts->prg[i].id == programid)
            ts->prg[i].nb_pids = 0;
}

static void clear_programs(MpegTSContext *ts)
{
    av_freep(&ts->prg);
    ts->nb_prg=0;
}

static void add_pat_entry(MpegTSContext *ts, unsigned int programid, unsigned int pid)
{
    struct Program *p;
    void *tmp = av_realloc(ts->prg, (ts->nb_prg+1)*sizeof(struct Program));
    if(!tmp)
        return;
    ts->prg = tmp;
    p = &ts->prg[ts->nb_prg];
    p->id = programid;
    p->pid = pid;
    p->nb_pids = 0;
    ts->nb_prg++;
}

static void add_pid_to_pmt(MpegTSContext *ts, unsigned int programid, unsigned int pid)
{
    int i;
    struct Program *p = NULL;
    for(i=0; i<ts->nb_prg; i++) {
        if(ts->prg[i].id == programid) {
            p = &ts->prg[i];
            break;
        }
    }
    if(!p)
        return;

    if(p->nb_pids >= MAX_PIDS_PER_PROGRAM)
        return;
    p->pids[p->nb_pids++] = pid;
}

static void set_pcr_pid(AVFormatContext *s, unsigned int programid, unsigned int pid)
{
    int i;
    for(i=0; i<s->nb_programs; i++) {
        if(s->programs[i]->id == programid) {
            s->programs[i]->pcr_pid = pid;
            break;
        }
    }
}

static void mpegts_close_filter(MpegTSContext *ts, MpegTSFilter *filter);

/**
 * @brief discard_pid() decides if the pid is to be discarded according
 *                      to caller's programs selection
 * @param ts    : - TS context
 * @param pid   : - pid
 * @return 1 if the pid is only comprised in programs that have .discard=AVDISCARD_ALL
 *         0 otherwise
 */
static int discard_pid(MpegTSContext *ts, unsigned int pid)
{
    int i, j, k;
    int used = 0, discarded = 0;
    struct Program *p;
    for(i=0; i<ts->nb_prg; i++) {
        p = &ts->prg[i];
        for(j=0; j<p->nb_pids; j++) {
            if(p->pids[j] != pid)
                continue;
            //is program with id p->id set to be discarded?
            for(k=0; k<ts->stream->nb_programs; k++) {
                if(ts->stream->programs[k]->id == p->id) {
                    if(ts->stream->programs[k]->discard == AVDISCARD_ALL)
                        discarded++;
                    else
                        used++;
                }
            }
        }
    }

    return !used && discarded;
}

//ugly forward declaration
static void mpegts_push_section(MpegTSFilter *filter, const uint8_t *section, int section_len);

/** \fn write_section_data(AVFormatContext*,MpegTSFilter*,const uint8_t*,int,int)
 *  Assemble PES packets out of TS packets, and then call the "section_cb"
 *  function when they are complete.
 *
 *  NOTE: "DVB Section" is DVB terminology for an MPEG PES packet.
 */
static void write_section_data(AVFormatContext *s, MpegTSFilter *tss1,
                               const uint8_t *buf, int buf_size, int is_start)
{
    MpegTSSectionFilter *tss = &tss1->u.section_filter;
    int len;

    assert(tss->section_buf);

    if (is_start) {
        memcpy(tss->section_buf, buf, buf_size);
        tss->section_index = buf_size;
        tss->section_h_size = -1;
        tss->end_of_section_reached = 0;
    } else {
        if (tss->end_of_section_reached)
            return;
        len = 4096 - tss->section_index;
        if (buf_size < len)
            len = buf_size;
        memcpy(tss->section_buf + tss->section_index, buf, len);
        tss->section_index += len;
    }

    if (tss->section_cb == mpegts_push_section) {
        SectionContext *sect = tss->opaque;
        sect->new_packet = 1;
    }
    while (!tss->end_of_section_reached) {
        /* compute section length if possible */
        if (tss->section_h_size == -1 && tss->section_index >= 3) {
            len = (AV_RB16(tss->section_buf + 1) & 0xfff) + 3;
            if (len > 4096)
                return;
            tss->section_h_size = len;
        }

        if (tss->section_h_size == -1 ||
            tss->section_index < tss->section_h_size)
        {
            if (tss1->pmt_chop_at_ts && tss->section_buf[0] == PMT_TID)
            {
                /* HACK!  To allow BBC IPTV streams with incomplete PMTs (they
                 * advertise a length of 383, but only send 182 bytes!), we
                 * will not wait for the remainder of the PMT, but accept just
                 * what is in the first TS payload, as this is enough to get
                 * playback, although some PIDs may be filtered out as a result
                 */
                tss->section_h_size = tss->section_index;
            }
            else
                break;
        }

        if (!tss->check_crc ||
            av_crc(av_crc_get_table(AV_CRC_32_IEEE), -1,
                   tss->section_buf, tss->section_h_size) == 0)
            tss->section_cb(tss1, tss->section_buf, tss->section_h_size);
        else
            av_log(s, AV_LOG_WARNING, "write_section_data: PID %#x CRC error\n", tss1->pid);

        if (tss->section_index > tss->section_h_size) {
            int left = tss->section_index - tss->section_h_size;
            memmove(tss->section_buf, tss->section_buf+tss->section_h_size,
                    left);
            tss->section_index = left;
            tss->section_h_size = -1;
        } else {
            tss->end_of_section_reached = 1;
        }
    }
}

static MpegTSFilter *mpegts_open_section_filter(MpegTSContext *ts, unsigned int pid,
                                         SectionCallback *section_cb, void *opaque,
                                         int check_crc)

{
    MpegTSFilter *filter = ts->pids[pid];
    MpegTSSectionFilter *sec;

    av_dlog(ts->stream, "Filter: pid=0x%x\n", pid);

    if (filter) {
#ifdef DEBUG
	av_log(ts->stream, AV_LOG_DEBUG, "Filter Already Exists\n");
#endif
        mpegts_close_filter(ts, filter);
    }

    if (pid >= NB_PID_MAX || ts->pids[pid])
        return NULL;
    filter = av_mallocz(sizeof(MpegTSFilter));
    if (!filter)
        return NULL;
    ts->pids[pid] = filter;
    filter->type = MPEGTS_SECTION;
    filter->pid = pid;
    filter->es_id = -1;
    filter->last_cc = -1;
    filter->pmt_chop_at_ts = 0;
    sec = &filter->u.section_filter;
    sec->section_cb = section_cb;
    sec->opaque = opaque;
    sec->section_buf = av_malloc(MAX_SECTION_SIZE);
    sec->check_crc = check_crc;
    if (!sec->section_buf) {
        av_free(filter);
        return NULL;
    }
    return filter;
}

static MpegTSFilter *mpegts_open_pes_filter(MpegTSContext *ts, unsigned int pid,
                                     PESCallback *pes_cb,
                                     void *opaque)
{
    MpegTSFilter *filter;
    MpegTSPESFilter *pes;

    if (pid >= NB_PID_MAX || ts->pids[pid])
        return NULL;
    filter = av_mallocz(sizeof(MpegTSFilter));
    if (!filter)
        return NULL;
    ts->pids[pid] = filter;
    filter->type = MPEGTS_PES;
    filter->pid = pid;
    filter->es_id = -1;
    filter->last_cc = -1;
    pes = &filter->u.pes_filter;
    pes->pes_cb = pes_cb;
    pes->opaque = opaque;
    return filter;
}

static void mpegts_close_filter(MpegTSContext *ts, MpegTSFilter *filter)
{
    int pid;

    if (!ts || !filter)
        return;

    pid = filter->pid;

#ifdef DEBUG
    av_log(NULL, AV_LOG_DEBUG, "Closing Filter: pid=0x%x\n", pid);
#endif
    if (filter == ts->pmt_filter)
    {
        av_log(NULL, AV_LOG_DEBUG, "Closing PMT Filter: pid=0x%x\n", pid);
        ts->pmt_filter = NULL;
    }
    if (filter == ts->pat_filter)
    {
        av_log(NULL, AV_LOG_DEBUG, "Closing PAT Filter: pid=0x%x\n", pid);
        ts->pat_filter = NULL;
    }

    if (filter->type == MPEGTS_SECTION)
        av_freep(&filter->u.section_filter.section_buf);
    else if (filter->type == MPEGTS_PES) {
        PESContext *pes = filter->u.pes_filter.opaque;
        av_freep(&pes->buffer);
        /* referenced private data will be freed later in
         * avformat_close_input */
        if (!((PESContext *)filter->u.pes_filter.opaque)->st) {
            av_freep(&filter->u.pes_filter.opaque);
        }
    }

    av_free(filter);
    ts->pids[pid] = NULL;
}

static int analyze(const uint8_t *buf, int size, int packet_size, int *index){
    int stat[TS_MAX_PACKET_SIZE];
    int i;
    int x=0;
    int best_score=0;

    memset(stat, 0, packet_size*sizeof(int));

    for(x=i=0; i<size-3; i++){
        if(buf[i] == 0x47 && !(buf[i+1] & 0x80) && buf[i+3] != 0x47){
            stat[x]++;
            if(stat[x] > best_score){
                best_score= stat[x];
                if(index) *index= x;
            }
        }

        x++;
        if(x == packet_size) x= 0;
    }

    return best_score;
}

/* autodetect fec presence. Must have at least 1024 bytes  */
static int get_packet_size(const uint8_t *buf, int size)
{
    int score, fec_score, dvhs_score;

    if (size < (TS_FEC_PACKET_SIZE * 5 + 1))
        return -1;

    score    = analyze(buf, size, TS_PACKET_SIZE, NULL);
    dvhs_score    = analyze(buf, size, TS_DVHS_PACKET_SIZE, NULL);
    fec_score= analyze(buf, size, TS_FEC_PACKET_SIZE, NULL);
//    av_log(NULL, AV_LOG_DEBUG, "score: %d, dvhs_score: %d, fec_score: %d \n", score, dvhs_score, fec_score);

    if     (score > fec_score && score > dvhs_score) return TS_PACKET_SIZE;
    else if(dvhs_score > score && dvhs_score > fec_score) return TS_DVHS_PACKET_SIZE;
    else if(score < fec_score && dvhs_score < fec_score) return TS_FEC_PACKET_SIZE;
    else                       return -1;
}

typedef struct SectionHeader {
    uint8_t tid;
    uint16_t id;
    uint8_t version;
    uint8_t sec_num;
    uint8_t last_sec_num;
} SectionHeader;

static inline int get8(const uint8_t **pp, const uint8_t *p_end)
{
    const uint8_t *p;
    int c;

    p = *pp;
    if (p >= p_end)
        return -1;
    c = *p++;
    *pp = p;
    return c;
}

static inline int get16(const uint8_t **pp, const uint8_t *p_end)
{
    const uint8_t *p;
    int c;

    p = *pp;
    if ((p + 1) >= p_end)
        return -1;
    c = AV_RB16(p);
    p += 2;
    *pp = p;
    return c;
}

/* read and allocate a DVB string preceded by its length */
static char *getstr8(const uint8_t **pp, const uint8_t *p_end)
{
    int len;
    const uint8_t *p;
    char *str;

    p = *pp;
    len = get8(&p, p_end);
    if (len < 0)
        return NULL;
    if ((p + len) > p_end)
        return NULL;
    str = av_malloc(len + 1);
    if (!str)
        return NULL;
    memcpy(str, p, len);
    str[len] = '\0';
    p += len;
    *pp = p;
    return str;
}

static int parse_section_header(SectionHeader *h,
                                const uint8_t **pp, const uint8_t *p_end)
{
    int val;

    val = get8(pp, p_end);
    if (val < 0)
        return -1;
    h->tid = val;
    *pp += 2;
    val = get16(pp, p_end);
    if (val < 0)
        return -1;
    h->id = val;
    val = get8(pp, p_end);
    if (val < 0)
        return -1;
    h->version = (val >> 1) & 0x1f;
    val = get8(pp, p_end);
    if (val < 0)
        return -1;
    h->sec_num = val;
    val = get8(pp, p_end);
    if (val < 0)
        return -1;
    h->last_sec_num = val;

#ifdef DEBUG
    av_log(NULL, AV_LOG_DEBUG, "sid=0x%x sec_num=%d/%d\n",
           h->id, h->sec_num, h->last_sec_num);
#endif
    return 0;
}

/* mpegts_push_section: return one or more tables.  The tables may not completely fill
   the packet and there may be stuffing bytes at the end.
   This is complicated because a single TS packet may result in several tables being
   produced.  We may have a "start" bit indicating, in effect, the end of a table but
   the rest of the TS packet after the start may be filled with one or more small tables.
*/
static void mpegts_push_section(MpegTSFilter *filter, const uint8_t *section, int section_len)
{
    SectionContext *sect = filter->u.section_filter.opaque;
    MpegTSContext *ts = sect->ts;
    SectionHeader header;
    AVPacket *pkt = ts->pkt;
    const uint8_t *p = section, *p_end = section + section_len - 4;

    if (parse_section_header(&header, &p, p_end) < 0)
    {
        av_log(NULL, AV_LOG_DEBUG, "Unable to parse header\n");
        return;
    }

    if (sect->new_packet && pkt && sect->st && pkt->size == 0) {
        int pktLen = section_len + 184; /* Add enough for a complete TS payload. */
        sect->new_packet = 0;
        av_free_packet(pkt);
        if (av_new_packet(pkt, pktLen) == 0) {
            memcpy(pkt->data, section, section_len);
            memset(pkt->data+section_len, 0xff, pktLen-section_len);
            pkt->stream_index = sect->st->index;
            ts->stop_parse = 1;
        }
    } else if (pkt->data) { /* We've already added at least one table. */
        uint8_t *data = pkt->data;
        int space = pkt->size;
        int table_size = 0;
        while (space > 3 + table_size) {
            table_size = (((data[1] & 0xf) << 8) | data[2]) + 3;
            if (table_size < space) {
                space -= table_size;
                data += table_size;
            } /* Otherwise we've got filler. */
        }
        if (space < section_len) {
            av_log(NULL, AV_LOG_DEBUG, "Insufficient space for additional packet\n");
            return;
        }
        memcpy(data, section, section_len);
   }
}

typedef struct {
    uint32_t stream_type;
    enum AVMediaType codec_type;
    enum AVCodecID codec_id;
} StreamType;

static const StreamType ISO_types[] = {
    { 0x01, AVMEDIA_TYPE_VIDEO, AV_CODEC_ID_MPEG2VIDEO },
    { 0x02, AVMEDIA_TYPE_VIDEO, AV_CODEC_ID_MPEG2VIDEO },
    { 0x03, AVMEDIA_TYPE_AUDIO,        AV_CODEC_ID_MP3 },
    { 0x04, AVMEDIA_TYPE_AUDIO,        AV_CODEC_ID_MP3 },
    { 0x0b, AVMEDIA_TYPE_DATA,     AV_CODEC_ID_DSMCC_B }, /* DVB_CAROUSEL_ID */
    { 0x0f, AVMEDIA_TYPE_AUDIO,        AV_CODEC_ID_AAC },
    { 0x10, AVMEDIA_TYPE_VIDEO,      AV_CODEC_ID_MPEG4 },
    { 0x11, AVMEDIA_TYPE_AUDIO,   AV_CODEC_ID_AAC_LATM }, /* LATM syntax */
    { 0x1b, AVMEDIA_TYPE_VIDEO,       AV_CODEC_ID_H264 },
    { 0x24, AVMEDIA_TYPE_VIDEO,       AV_CODEC_ID_HEVC },
    { 0xd1, AVMEDIA_TYPE_VIDEO,      AV_CODEC_ID_DIRAC },
    { 0xea, AVMEDIA_TYPE_VIDEO,        AV_CODEC_ID_VC1 },
    { 0 },
};

static const StreamType HDMV_types[] = {
    { 0x80, AVMEDIA_TYPE_AUDIO, AV_CODEC_ID_PCM_BLURAY },
    { 0x81, AVMEDIA_TYPE_AUDIO, AV_CODEC_ID_AC3 },
    { 0x82, AVMEDIA_TYPE_AUDIO, AV_CODEC_ID_DTS },
    { 0x83, AVMEDIA_TYPE_AUDIO, AV_CODEC_ID_TRUEHD },
    { 0x84, AVMEDIA_TYPE_AUDIO, AV_CODEC_ID_EAC3 },
    { 0x85, AVMEDIA_TYPE_AUDIO, AV_CODEC_ID_DTS }, /* DTS HD */
    { 0x86, AVMEDIA_TYPE_AUDIO, AV_CODEC_ID_DTS }, /* DTS HD MASTER*/
    { 0xa1, AVMEDIA_TYPE_AUDIO, AV_CODEC_ID_EAC3 }, /* E-AC3 Secondary Audio */
    { 0xa2, AVMEDIA_TYPE_AUDIO, AV_CODEC_ID_DTS },  /* DTS Express Secondary Audio */
    { 0x90, AVMEDIA_TYPE_SUBTITLE, AV_CODEC_ID_HDMV_PGS_SUBTITLE },
    { 0 },
};

/* ATSC ? */
static const StreamType MISC_types[] = {
    { 0x81, AVMEDIA_TYPE_AUDIO,     AV_CODEC_ID_AC3 },
    { 0x87, AVMEDIA_TYPE_AUDIO,     AV_CODEC_ID_EAC3 },
    { 0x8a, AVMEDIA_TYPE_AUDIO,     AV_CODEC_ID_DTS },
    { 0x100, AVMEDIA_TYPE_SUBTITLE, AV_CODEC_ID_DVB_SUBTITLE },
    { 0x101, AVMEDIA_TYPE_DATA,     AV_CODEC_ID_DVB_VBI },
    { 0 },
};

static const StreamType REGD_types[] = {
    { MKTAG('d','r','a','c'), AVMEDIA_TYPE_VIDEO, AV_CODEC_ID_DIRAC },
    { MKTAG('A','C','-','3'), AVMEDIA_TYPE_AUDIO,   AV_CODEC_ID_AC3 },
    { MKTAG('B','S','S','D'), AVMEDIA_TYPE_AUDIO, AV_CODEC_ID_S302M },
    { MKTAG('D','T','S','1'), AVMEDIA_TYPE_AUDIO,   AV_CODEC_ID_DTS },
    { MKTAG('D','T','S','2'), AVMEDIA_TYPE_AUDIO,   AV_CODEC_ID_DTS },
    { MKTAG('D','T','S','3'), AVMEDIA_TYPE_AUDIO,   AV_CODEC_ID_DTS },
    { MKTAG('V','C','-','1'), AVMEDIA_TYPE_VIDEO,   AV_CODEC_ID_VC1 },
    { 0 },
};

/* descriptor present */
static const StreamType DESC_types[] = {
    { 0x6a, AVMEDIA_TYPE_AUDIO,             AV_CODEC_ID_AC3 }, /* AC-3 descriptor */
    { 0x7a, AVMEDIA_TYPE_AUDIO,            AV_CODEC_ID_EAC3 }, /* E-AC-3 descriptor */
    { 0x7b, AVMEDIA_TYPE_AUDIO,             AV_CODEC_ID_DTS },
    { 0x13, AVMEDIA_TYPE_DATA,          AV_CODEC_ID_DSMCC_B }, /* DVB_CAROUSEL_ID */
    { 0x45, AVMEDIA_TYPE_DATA,          AV_CODEC_ID_DVB_VBI }, /* DVB_VBI_DATA_ID */
    { 0x46, AVMEDIA_TYPE_DATA,          AV_CODEC_ID_DVB_VBI }, /* DVB_VBI_TELETEXT_ID */ //FixMe type subtilte
    { 0x56, AVMEDIA_TYPE_SUBTITLE, AV_CODEC_ID_DVB_TELETEXT },
    { 0x59, AVMEDIA_TYPE_SUBTITLE, AV_CODEC_ID_DVB_SUBTITLE }, /* subtitling descriptor */
    { 0 },
};

/* component tags */
static const StreamType COMPONENT_TAG_types[] = {
    { 0x0a, AVMEDIA_TYPE_AUDIO,        AV_CODEC_ID_MP3 },
    { 0x52, AVMEDIA_TYPE_VIDEO, AV_CODEC_ID_MPEG2VIDEO },
};

static void mpegts_find_stream_type(AVStream *st,
                                    uint32_t stream_type, const StreamType *types)
{
    for (; types->stream_type; types++) {
        if (stream_type == types->stream_type) {
            st->codecpar->codec_type = types->codec_type;
            st->codecpar->codec_id   = types->codec_id;
            st->internal->request_probe        = 0;
            return;
        }
    }
}

static void mpegts_find_stream_type_pmt(pmt_entry_t *st,
                                    uint32_t stream_type, const StreamType *types)
{
    for (; types->stream_type; types++) {
        if (stream_type == types->stream_type) {
            st->codec_type = types->codec_type;
            st->codec_id   = types->codec_id;
            return;
        }
    }
}

static int mpegts_set_stream_info(AVStream *st, PESContext *pes,
                                  uint32_t stream_type, uint32_t prog_reg_desc)
{
    int old_codec_type= st->codecpar->codec_type;
    int old_codec_id  = st->codecpar->codec_id;
    avpriv_set_pts_info(st, 33, 1, 90000);
    st->priv_data = pes;
    st->codecpar->codec_type = AVMEDIA_TYPE_DATA;
    st->codecpar->codec_id   = AV_CODEC_ID_NONE;
    st->need_parsing = AVSTREAM_PARSE_FULL;
    pes->st = st;
    pes->stream_type = stream_type;

    av_log(pes->stream, AV_LOG_DEBUG,
           "stream=%d stream_type=%x pid=%x prog_reg_desc=%.4s\n",
           st->index, pes->stream_type, pes->pid, (char*)&prog_reg_desc);

    st->codecpar->codec_tag = pes->stream_type;

    mpegts_find_stream_type(st, pes->stream_type, ISO_types);
    if ((prog_reg_desc == AV_RL32("HDMV") ||
         prog_reg_desc == AV_RL32("HDPR")) &&
        st->codecpar->codec_id == AV_CODEC_ID_NONE) {
        mpegts_find_stream_type(st, pes->stream_type, HDMV_types);
        if (pes->stream_type == 0x83) {
            // HDMV TrueHD streams also contain an AC3 coded version of the
            // audio track - add a second stream for this
            AVStream *sub_st;
            // priv_data cannot be shared between streams
            PESContext *sub_pes = av_malloc(sizeof(*sub_pes));
            if (!sub_pes)
                return AVERROR(ENOMEM);
            memcpy(sub_pes, pes, sizeof(*sub_pes));

            sub_st = avformat_new_stream(pes->stream, NULL);
            if (!sub_st) {
                av_free(sub_pes);
                return AVERROR(ENOMEM);
            }

            sub_st->id = pes->pid;
            avpriv_set_pts_info(sub_st, 33, 1, 90000);
            sub_st->priv_data = sub_pes;
            sub_st->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
            sub_st->codecpar->codec_id   = AV_CODEC_ID_AC3;
            sub_st->need_parsing = AVSTREAM_PARSE_FULL;
            sub_pes->sub_st = pes->sub_st = sub_st;
        }
    }
    if (st->codecpar->codec_id == AV_CODEC_ID_NONE)
        mpegts_find_stream_type(st, pes->stream_type, MISC_types);
    if (st->codecpar->codec_id == AV_CODEC_ID_NONE){
        st->codecpar->codec_id  = old_codec_id;
        st->codecpar->codec_type= old_codec_type;
    }

    return 0;
}

static void new_pes_packet(PESContext *pes, AVPacket *pkt)
{
    av_packet_from_data(pkt, pes->buffer, pes->data_index);

    if(pes->total_size != MAX_PES_PAYLOAD &&
       pes->pes_header_size + pes->data_index != pes->total_size + PES_START_SIZE) {
        av_log(pes->stream, AV_LOG_WARNING, "PES packet size mismatch\n");
        pes->flags |= AV_PKT_FLAG_CORRUPT;
    }
    memset(pkt->data+pkt->size, 0, AV_INPUT_BUFFER_PADDING_SIZE);

    // Separate out the AC3 substream from an HDMV combined TrueHD/AC3 PID
    if (pes->sub_st && pes->stream_type == 0x83 && pes->extended_stream_id == 0x76)
        pkt->stream_index = pes->sub_st->index;
    else
        pkt->stream_index = pes->st->index;
    pkt->pts = pes->pts;
    pkt->dts = pes->dts;
    /* store position of first TS packet of this PES packet */
    pkt->pos = pes->ts_packet_pos;
    pkt->flags = pes->flags;

    /* reset pts values */
    pes->pts = AV_NOPTS_VALUE;
    pes->dts = AV_NOPTS_VALUE;
    pes->buffer = NULL;
    pes->data_index = 0;
    pes->flags = 0;
}

static int read_sl_header(PESContext *pes, SLConfigDescr *sl, const uint8_t *buf, int buf_size)
{
    GetBitContext gb;
    int au_start_flag = 0, au_end_flag = 0, ocr_flag = 0, idle_flag = 0;
    int padding_flag = 0, padding_bits = 0, inst_bitrate_flag = 0;
    int dts_flag = -1, cts_flag = -1;
    int64_t dts = AV_NOPTS_VALUE, cts = AV_NOPTS_VALUE;

    init_get_bits(&gb, buf, buf_size*8);

    if (sl->use_au_start)
        au_start_flag = get_bits1(&gb);
    if (sl->use_au_end)
        au_end_flag = get_bits1(&gb);
    if (!sl->use_au_start && !sl->use_au_end)
        au_start_flag = au_end_flag = 1;
    if (sl->ocr_len > 0)
        ocr_flag = get_bits1(&gb);
    if (sl->use_idle)
        idle_flag = get_bits1(&gb);
    if (sl->use_padding)
        padding_flag = get_bits1(&gb);
    if (padding_flag)
        padding_bits = get_bits(&gb, 3);

    if (!idle_flag && (!padding_flag || padding_bits != 0)) {
        if (sl->packet_seq_num_len)
            skip_bits_long(&gb, sl->packet_seq_num_len);
        if (sl->degr_prior_len)
            if (get_bits1(&gb))
                skip_bits(&gb, sl->degr_prior_len);
        if (ocr_flag)
            skip_bits_long(&gb, sl->ocr_len);
        if (au_start_flag) {
            if (sl->use_rand_acc_pt)
                get_bits1(&gb);
            if (sl->au_seq_num_len > 0)
                skip_bits_long(&gb, sl->au_seq_num_len);
            if (sl->use_timestamps) {
                dts_flag = get_bits1(&gb);
                cts_flag = get_bits1(&gb);
            }
        }
        if (sl->inst_bitrate_len)
            inst_bitrate_flag = get_bits1(&gb);
        if (dts_flag == 1)
            dts = get_bits64(&gb, sl->timestamp_len);
        if (cts_flag == 1)
            cts = get_bits64(&gb, sl->timestamp_len);
        if (sl->au_len > 0)
            skip_bits_long(&gb, sl->au_len);
        if (inst_bitrate_flag)
            skip_bits_long(&gb, sl->inst_bitrate_len);
    }

    if (dts != AV_NOPTS_VALUE)
        pes->dts = dts;
    if (cts != AV_NOPTS_VALUE)
        pes->pts = cts;

    if (sl->timestamp_len && sl->timestamp_res)
        avpriv_set_pts_info(pes->st, sl->timestamp_len, 1, sl->timestamp_res);

    return (get_bits_count(&gb) + 7) >> 3;
}

/* return non zero if a packet could be constructed */
static int mpegts_push_data(MpegTSFilter *filter,
                            const uint8_t *buf, int buf_size, int is_start,
                            int64_t pos)
{
    PESContext *pes = filter->u.pes_filter.opaque;
    MpegTSContext *ts = pes->ts;
    const uint8_t *p;
    int len, code;

    if(!ts || !ts->pkt)
        return 0;

    if (is_start) {
        if (pes->state == MPEGTS_PAYLOAD && pes->data_index > 0) {
            new_pes_packet(pes, ts->pkt);
            ts->stop_parse = 1;
        }
        pes->state = MPEGTS_HEADER;
        pes->data_index = 0;
        pes->ts_packet_pos = pos;
    }
    p = buf;
    while (buf_size > 0) {
        switch(pes->state) {
        case MPEGTS_HEADER:
            len = PES_START_SIZE - pes->data_index;
            if (len > buf_size)
                len = buf_size;
            memcpy(pes->header + pes->data_index, p, len);
            pes->data_index += len;
            p += len;
            buf_size -= len;
            if (pes->data_index == PES_START_SIZE) {
                /* we got all the PES or section header. We can now
                   decide */
                if (pes->header[0] == 0x00 && pes->header[1] == 0x00 &&
                    pes->header[2] == 0x01) {
                    /* it must be an mpeg2 PES stream */
                    code = pes->header[3] | 0x100;
                    av_dlog(pes->stream, "pid=%x pes_code=%#x\n", pes->pid, code);

                    if ((pes->st && pes->st->discard == AVDISCARD_ALL &&
                         (!pes->sub_st || pes->sub_st->discard == AVDISCARD_ALL)) ||
                        code == 0x1be) /* padding_stream */
                        goto skip;

                    /* stream not present in PMT */
                    if (!pes->st) {
                        pes->st = avformat_new_stream(ts->stream, NULL);
                        if (!pes->st)
                            return AVERROR(ENOMEM);
                        pes->st->id = pes->pid;
                        mpegts_set_stream_info(pes->st, pes, 0, 0);
                    }

                    pes->total_size = AV_RB16(pes->header + 4);
                    /* NOTE: a zero total size means the PES size is
                       unbounded */
                    if (!pes->total_size)
                        pes->total_size = MAX_PES_PAYLOAD;

                    /* allocate pes buffer */
                    pes->buffer = av_malloc(pes->total_size+AV_INPUT_BUFFER_PADDING_SIZE);
                    if (!pes->buffer)
                        return AVERROR(ENOMEM);

                    if (code != 0x1bc && code != 0x1bf && /* program_stream_map, private_stream_2 */
                        code != 0x1f0 && code != 0x1f1 && /* ECM, EMM */
                        code != 0x1ff && code != 0x1f2 && /* program_stream_directory, DSMCC_stream */
                        code != 0x1f8) {                  /* ITU-T Rec. H.222.1 type E stream */
                        pes->state = MPEGTS_PESHEADER;
                        if (pes->st->codecpar->codec_id == AV_CODEC_ID_NONE && !pes->st->internal->request_probe) {
                            av_dlog(pes->stream, "pid=%x stream_type=%x probing\n",
                                    pes->pid, pes->stream_type);
                            pes->st->internal->request_probe= 1;
                        }
                    } else {
                        pes->state = MPEGTS_PAYLOAD;
                        pes->data_index = 0;
                    }
                } else {
                    /* otherwise, it should be a table */
                    /* skip packet */
                skip:
                    pes->state = MPEGTS_SKIP;
                    continue;
                }
            }
            break;
            /**********************************************/
            /* PES packing parsing */
        case MPEGTS_PESHEADER:
            len = PES_HEADER_SIZE - pes->data_index;
            if (len < 0)
                return -1;
            if (len > buf_size)
                len = buf_size;
            memcpy(pes->header + pes->data_index, p, len);
            pes->data_index += len;
            p += len;
            buf_size -= len;
            if (pes->data_index == PES_HEADER_SIZE) {
                pes->pes_header_size = pes->header[8] + 9;
                pes->state = MPEGTS_PESHEADER_FILL;
            }
            break;
        case MPEGTS_PESHEADER_FILL:
            len = pes->pes_header_size - pes->data_index;
            if (len < 0)
                return -1;
            if (len > buf_size)
                len = buf_size;
            memcpy(pes->header + pes->data_index, p, len);
            pes->data_index += len;
            p += len;
            buf_size -= len;
            if (pes->data_index == pes->pes_header_size) {
                const uint8_t *r;
                unsigned int flags, pes_ext, skip;

                flags = pes->header[7];
                r = pes->header + 9;
                pes->pts = AV_NOPTS_VALUE;
                pes->dts = AV_NOPTS_VALUE;
                if ((flags & 0xc0) == 0x80) {
                    pes->dts = pes->pts = ff_parse_pes_pts(r);
                    r += 5;
                } else if ((flags & 0xc0) == 0xc0) {
                    pes->pts = ff_parse_pes_pts(r);
                    r += 5;
                    pes->dts = ff_parse_pes_pts(r);
                    r += 5;
                }
                pes->extended_stream_id = -1;
                if (flags & 0x01) { /* PES extension */
                    pes_ext = *r++;
                    /* Skip PES private data, program packet sequence counter and P-STD buffer */
                    skip = (pes_ext >> 4) & 0xb;
                    skip += skip & 0x9;
                    r += skip;
                    if ((pes_ext & 0x41) == 0x01 &&
                        (r + 2) <= (pes->header + pes->pes_header_size)) {
                        /* PES extension 2 */
                        if ((r[0] & 0x7f) > 0 && (r[1] & 0x80) == 0)
                            pes->extended_stream_id = r[1];
                    }
                }

                /* we got the full header. We parse it and get the payload */
                pes->state = MPEGTS_PAYLOAD;
                pes->data_index = 0;
                if (pes->stream_type == 0x12 && buf_size > 0) {
                    int sl_header_bytes = read_sl_header(pes, &pes->sl, p, buf_size);
                    pes->pes_header_size += sl_header_bytes;
                    p += sl_header_bytes;
                    buf_size -= sl_header_bytes;
                }
            }
            break;
        case MPEGTS_PAYLOAD:
            if (buf_size > 0 && pes->buffer) {
                if (pes->data_index > 0 && pes->data_index+buf_size > pes->total_size) {
                    new_pes_packet(pes, ts->pkt);
                    pes->total_size = MAX_PES_PAYLOAD;
                    pes->buffer = av_malloc(pes->total_size+AV_INPUT_BUFFER_PADDING_SIZE);
                    if (!pes->buffer)
                        return AVERROR(ENOMEM);
                    ts->stop_parse = 1;
                } else if (pes->data_index == 0 && buf_size > pes->total_size) {
                    // pes packet size is < ts size packet and pes data is padded with 0xff
                    // not sure if this is legal in ts but see issue #2392
                    buf_size = pes->total_size;
                }
                memcpy(pes->buffer+pes->data_index, p, buf_size);
                pes->data_index += buf_size;
            }
            buf_size = 0;
            /* emit complete packets with known packet size
             * decreases demuxer delay for infrequent packets like subtitles from
             * a couple of seconds to milliseconds for properly muxed files.
             * total_size is the number of bytes following pes_packet_length
             * in the pes header, i.e. not counting the first PES_START_SIZE bytes */
            if (!ts->stop_parse && pes->total_size < MAX_PES_PAYLOAD &&
                pes->pes_header_size + pes->data_index == pes->total_size + PES_START_SIZE) {
                ts->stop_parse = 1;
                new_pes_packet(pes, ts->pkt);
            }
            break;
        case MPEGTS_SKIP:
            buf_size = 0;
            break;
        }
    }

    return 0;
}

static PESContext *add_pes_stream(MpegTSContext *ts, int pid, int pcr_pid)
{
    MpegTSFilter *tss = ts->pids[pid];
    PESContext *pes = 0;
    if (tss) { /* filter already exists */
        if (tss->type == MPEGTS_PES)
            pes = (PESContext*) tss->u.pes_filter.opaque;
        /* otherwise, kill it, and start a new stream */
        mpegts_close_filter(ts, tss);
    }

    /* create a PES context */
    if (!(pes=av_mallocz(sizeof(PESContext)))) {
        av_log(NULL, AV_LOG_ERROR, "Error: av_mallocz() failed in add_pes_stream");
        return 0;
    }
    pes->ts = ts;
    pes->stream = ts->stream;
    pes->pid = pid;
    pes->pcr_pid = pcr_pid;
    pes->state = MPEGTS_SKIP;
    pes->pts = AV_NOPTS_VALUE;
    pes->dts = AV_NOPTS_VALUE;
    tss = mpegts_open_pes_filter(ts, pid, mpegts_push_data, pes);
    if (!tss) {
        av_free(pes);
        av_log(NULL, AV_LOG_ERROR, "Error: unable to open "
               "mpegts PES filter in add_pes_stream");
        return 0;
    }
    return pes;
}

#define MAX_LEVEL 4
typedef struct {
    AVFormatContext *s;
    AVIOContext pb;
    Mp4Descr *descr;
    Mp4Descr *active_descr;
    int descr_count;
    int max_descr_count;
    int level;
} MP4DescrParseContext;

static int init_MP4DescrParseContext(
    MP4DescrParseContext *d, AVFormatContext *s, const uint8_t *buf,
    unsigned size, Mp4Descr *descr, int max_descr_count)
{
    int ret;
    if (size > (1<<30))
        return AVERROR_INVALIDDATA;

    if ((ret = ffio_init_context(&d->pb, (unsigned char*)buf, size, 0,
                          NULL, NULL, NULL, NULL)) < 0)
        return ret;

    d->s = s;
    d->level = 0;
    d->descr_count = 0;
    d->descr = descr;
    d->active_descr = NULL;
    d->max_descr_count = max_descr_count;

    return 0;
}

static void update_offsets(AVIOContext *pb, int64_t *off, int *len) {
    int64_t new_off = avio_tell(pb);
    (*len) -= new_off - *off;
    *off = new_off;
}

static int parse_mp4_descr(MP4DescrParseContext *d, int64_t off, int len,
                           int target_tag);

static int parse_mp4_descr_arr(MP4DescrParseContext *d, int64_t off, int len)
{
    while (len > 0) {
        if (parse_mp4_descr(d, off, len, 0) < 0)
            return -1;
        update_offsets(&d->pb, &off, &len);
    }
    return 0;
}

static int parse_MP4IODescrTag(MP4DescrParseContext *d, int64_t off, int len)
{
    avio_rb16(&d->pb); // ID
    avio_r8(&d->pb);
    avio_r8(&d->pb);
    avio_r8(&d->pb);
    avio_r8(&d->pb);
    avio_r8(&d->pb);
    update_offsets(&d->pb, &off, &len);
    return parse_mp4_descr_arr(d, off, len);
}

static int parse_MP4ODescrTag(MP4DescrParseContext *d, int64_t off, int len)
{
    int id_flags;
    if (len < 2)
        return 0;
    id_flags = avio_rb16(&d->pb);
    if (!(id_flags & 0x0020)) { //URL_Flag
        update_offsets(&d->pb, &off, &len);
        return parse_mp4_descr_arr(d, off, len); //ES_Descriptor[]
    } else {
        return 0;
    }
}

static int parse_MP4ESDescrTag(MP4DescrParseContext *d, int64_t off, int len)
{
    int es_id = 0;
    if (d->descr_count >= d->max_descr_count)
        return -1;
    ff_mp4_parse_es_descr(&d->pb, &es_id);
    d->active_descr = d->descr + (d->descr_count++);

    d->active_descr->es_id = es_id;
    update_offsets(&d->pb, &off, &len);
    parse_mp4_descr(d, off, len, MP4DecConfigDescrTag);
    update_offsets(&d->pb, &off, &len);
    if (len > 0)
        parse_mp4_descr(d, off, len, MP4SLDescrTag);
    d->active_descr = NULL;
    return 0;
}

static int parse_MP4DecConfigDescrTag(MP4DescrParseContext *d, int64_t off, int len)
{
    Mp4Descr *descr = d->active_descr;
    if (!descr)
        return -1;
    d->active_descr->dec_config_descr = av_malloc(len);
    if (!descr->dec_config_descr)
        return AVERROR(ENOMEM);
    descr->dec_config_descr_len = len;
    avio_read(&d->pb, descr->dec_config_descr, len);
    return 0;
}

static int parse_MP4SLDescrTag(MP4DescrParseContext *d, int64_t off, int len)
{
    Mp4Descr *descr = d->active_descr;
    int predefined;
    if (!descr)
        return -1;

    predefined = avio_r8(&d->pb);
    if (!predefined) {
        int lengths;
        int flags = avio_r8(&d->pb);
        descr->sl.use_au_start       = !!(flags & 0x80);
        descr->sl.use_au_end         = !!(flags & 0x40);
        descr->sl.use_rand_acc_pt    = !!(flags & 0x20);
        descr->sl.use_padding        = !!(flags & 0x08);
        descr->sl.use_timestamps     = !!(flags & 0x04);
        descr->sl.use_idle           = !!(flags & 0x02);
        descr->sl.timestamp_res      = avio_rb32(&d->pb);
                                       avio_rb32(&d->pb);
        descr->sl.timestamp_len      = avio_r8(&d->pb);
        descr->sl.ocr_len            = avio_r8(&d->pb);
        descr->sl.au_len             = avio_r8(&d->pb);
        descr->sl.inst_bitrate_len   = avio_r8(&d->pb);
        lengths                      = avio_rb16(&d->pb);
        descr->sl.degr_prior_len     = lengths >> 12;
        descr->sl.au_seq_num_len     = (lengths >> 7) & 0x1f;
        descr->sl.packet_seq_num_len = (lengths >> 2) & 0x1f;
    } else {
        avpriv_report_missing_feature(d->s, "Predefined SLConfigDescriptor");
    }
    return 0;
}

static int parse_mp4_descr(MP4DescrParseContext *d, int64_t off, int len,
                           int target_tag) {
    int tag;
    int len1 = ff_mp4_read_descr(d->s, &d->pb, &tag);
    update_offsets(&d->pb, &off, &len);
    if (len < 0 || len1 > len || len1 <= 0) {
        av_log(d->s, AV_LOG_ERROR, "Tag %x length violation new length %d bytes remaining %d\n", tag, len1, len);
        return -1;
    }

    if (d->level++ >= MAX_LEVEL) {
        av_log(d->s, AV_LOG_ERROR, "Maximum MP4 descriptor level exceeded\n");
        goto done;
    }

    if (target_tag && tag != target_tag) {
        av_log(d->s, AV_LOG_ERROR, "Found tag %x expected %x\n", tag, target_tag);
        goto done;
    }

    switch (tag) {
    case MP4IODescrTag:
        parse_MP4IODescrTag(d, off, len1);
        break;
    case MP4ODescrTag:
        parse_MP4ODescrTag(d, off, len1);
        break;
    case MP4ESDescrTag:
        parse_MP4ESDescrTag(d, off, len1);
        break;
    case MP4DecConfigDescrTag:
        parse_MP4DecConfigDescrTag(d, off, len1);
        break;
    case MP4SLDescrTag:
        parse_MP4SLDescrTag(d, off, len1);
        break;
    }

done:
    d->level--;
    avio_seek(&d->pb, off + len1, SEEK_SET);
    return 0;
}

static int mp4_read_iods(AVFormatContext *s, const uint8_t *buf, unsigned size,
                         Mp4Descr *descr, int *descr_count, int max_descr_count)
{
    MP4DescrParseContext d;
    if (init_MP4DescrParseContext(&d, s, buf, size, descr, max_descr_count) < 0)
        return -1;

    parse_mp4_descr(&d, avio_tell(&d.pb), size, MP4IODescrTag);

    *descr_count = d.descr_count;
    return 0;
}

static int mp4_read_od(AVFormatContext *s, const uint8_t *buf, unsigned size,
                       Mp4Descr *descr, int *descr_count, int max_descr_count)
{
    MP4DescrParseContext d;
    if (init_MP4DescrParseContext(&d, s, buf, size, descr, max_descr_count) < 0)
        return -1;

    parse_mp4_descr_arr(&d, avio_tell(&d.pb), size);

    *descr_count = d.descr_count;
    return 0;
}

static void m4sl_cb(MpegTSFilter *filter, const uint8_t *section, int section_len)
{
    MpegTSContext *ts = filter->u.section_filter.opaque;
    SectionHeader h;
    const uint8_t *p, *p_end;
    AVIOContext pb;
    Mp4Descr mp4_descr[MAX_MP4_DESCR_COUNT] = {{ 0 }};
    int mp4_descr_count = 0;
    int i, pid;
    AVFormatContext *s = ts->stream;

    p_end = section + section_len - 4;
    p = section;
    if (parse_section_header(&h, &p, p_end) < 0)
        return;
    if (h.tid != M4OD_TID)
        return;

    mp4_read_od(s, p, (unsigned)(p_end - p), mp4_descr, &mp4_descr_count, MAX_MP4_DESCR_COUNT);

    for (pid = 0; pid < NB_PID_MAX; pid++) {
        if (!ts->pids[pid])
             continue;
        for (i = 0; i < mp4_descr_count; i++) {
            PESContext *pes;
            AVStream *st;
            if (ts->pids[pid]->es_id != mp4_descr[i].es_id)
                continue;
            if (!(ts->pids[pid] && ts->pids[pid]->type == MPEGTS_PES)) {
                av_log(s, AV_LOG_ERROR, "pid %x is not PES\n", pid);
                continue;
            }
            pes = ts->pids[pid]->u.pes_filter.opaque;
            st = pes->st;
            if (!st) {
                continue;
            }

            pes->sl = mp4_descr[i].sl;

            ffio_init_context(&pb, mp4_descr[i].dec_config_descr,
                              mp4_descr[i].dec_config_descr_len, 0, NULL, NULL, NULL, NULL);
            ff_mp4_read_dec_config_descr(s, st, &pb);
            if (st->codecpar->codec_id == AV_CODEC_ID_AAC &&
                st->codecpar->extradata_size > 0)
                st->need_parsing = 0;
            if (st->codecpar->codec_id == AV_CODEC_ID_H264 &&
                st->codecpar->extradata_size > 0)
                st->need_parsing = 0;

            if (st->codecpar->codec_id <= AV_CODEC_ID_NONE) {
            } else if (st->codecpar->codec_id < AV_CODEC_ID_FIRST_AUDIO) {
                st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
            } else if (st->codecpar->codec_id < AV_CODEC_ID_FIRST_SUBTITLE) {
                st->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
            } else if (st->codecpar->codec_id < AV_CODEC_ID_FIRST_UNKNOWN) {
                st->codecpar->codec_type = AVMEDIA_TYPE_SUBTITLE;
            }
        }
    }
    for (i = 0; i < mp4_descr_count; i++)
        av_free(mp4_descr[i].dec_config_descr);
}

int ff_parse_mpeg2_descriptor(AVFormatContext *fc, pmt_entry_t *item, int stream_type,
                              const uint8_t **pp, const uint8_t *desc_list_end,
                              Mp4Descr *mp4_descr, int mp4_descr_count, int pid,
                              MpegTSContext *ts, dvb_caption_info_t *dvbci)
{
    const uint8_t *desc_end;
    int desc_len, desc_tag, desc_es_id;
    char *language;
    int i;

    desc_tag = get8(pp, desc_list_end);
    if (desc_tag < 0)
        return -1;
    desc_len = get8(pp, desc_list_end);
    if (desc_len < 0)
        return -1;
    desc_end = *pp + desc_len;
    if (desc_end > desc_list_end)
        return -1;

    av_dlog(fc, "tag: 0x%02x len=%d\n", desc_tag, desc_len);

    if (item->codec_id == AV_CODEC_ID_NONE &&
        stream_type == STREAM_TYPE_PRIVATE_DATA)
    {
        mpegts_find_stream_type_pmt(item, desc_tag, DESC_types);

        if (item->codec_id != AV_CODEC_ID_NONE)
            stream_type = 0;
    }

    language = dvbci->language;

    switch(desc_tag) {
    case 0x02: /* video stream descriptor */
        if (get8(pp, desc_end) & 0x1) {
            dvbci->disposition |= AV_DISPOSITION_STILL_IMAGE;
        }
        break;
#if 0
    case 0x1E: /* SL descriptor */
        desc_es_id = get16(pp, desc_end);
        if (ts && ts->pids[pid])
            ts->pids[pid]->es_id = desc_es_id;
        for (i = 0; i < mp4_descr_count; i++)
        if (mp4_descr[i].dec_config_descr_len &&
            mp4_descr[i].es_id == desc_es_id) {
            AVIOContext pb;
            ffio_init_context(&pb, mp4_descr[i].dec_config_descr,
                          mp4_descr[i].dec_config_descr_len, 0, NULL, NULL, NULL, NULL);
            ff_mp4_read_dec_config_descr(fc, st, &pb);
            if (item->codec_id == AV_CODEC_ID_AAC &&
                st->codecpar->extradata_size > 0)
                st->need_parsing = 0;
            if (item->codec_id == AV_CODEC_ID_MPEG4SYSTEMS)
                mpegts_open_section_filter(ts, pid, m4sl_cb, ts, 1);
        }
        break;
    case 0x1F: /* FMC descriptor */
        get16(pp, desc_end);
        if (mp4_descr_count > 0 && (item->codec_id == AV_CODEC_ID_AAC_LATM || st->internal->request_probe>0) &&
            mp4_descr->dec_config_descr_len && mp4_descr->es_id == pid) {
            AVIOContext pb;
            ffio_init_context(&pb, mp4_descr->dec_config_descr,
                          mp4_descr->dec_config_descr_len, 0, NULL, NULL, NULL, NULL);
            ff_mp4_read_dec_config_descr(fc, st, &pb);
            if (item->codec_id == AV_CODEC_ID_AAC &&
                st->codecpar->extradata_size > 0){
                st->internal->request_probe= st->need_parsing = 0;
                st->codecpar->codec_type= AVMEDIA_TYPE_AUDIO;
            }
        }
        break;
#endif
    case 0x56: /* DVB teletext descriptor */
        language[0] = get8(pp, desc_end);
        language[1] = get8(pp, desc_end);
        language[2] = get8(pp, desc_end);
        language[3] = 0;
        break;
    case 0x59: /* subtitling descriptor */
        language[0] = get8(pp, desc_end);
        language[1] = get8(pp, desc_end);
        language[2] = get8(pp, desc_end);
        language[3] = 0;
        get8(pp, desc_end);

#if 0
        /* hearing impaired subtitles detection */
        switch(get8(pp, desc_end)) {
        case 0x20: /* DVB subtitles (for the hard of hearing) with no monitor aspect ratio criticality */
        case 0x21: /* DVB subtitles (for the hard of hearing) for display on 4:3 aspect ratio monitor */
        case 0x22: /* DVB subtitles (for the hard of hearing) for display on 16:9 aspect ratio monitor */
        case 0x23: /* DVB subtitles (for the hard of hearing) for display on 2.21:1 aspect ratio monitor */
        case 0x24: /* DVB subtitles (for the hard of hearing) for display on a high definition monitor */
        case 0x25: /* DVB subtitles (for the hard of hearing) with plano-stereoscopic disparity for display on a high definition monitor */
            st->disposition |= AV_DISPOSITION_HEARING_IMPAIRED;
            break;
        }
#endif

        dvbci->comp_page   = get16(pp, desc_end);
        dvbci->anc_page    = get16(pp, desc_end);
        dvbci->sub_id = (dvbci->anc_page << 16) | dvbci->comp_page;

#if 0
        if (st->codecpar->extradata) {
            if (st->codecpar->extradata_size == 4 && memcmp(st->codecpar->extradata, *pp, 4))
                av_log_ask_for_sample(fc, "DVB sub with multiple IDs\n");
        } else {
            st->codecpar->extradata = av_malloc(4 + AV_INPUT_BUFFER_PADDING_SIZE);
            if (st->codecpar->extradata) {
                st->codecpar->extradata_size = 4;
                memcpy(st->codecpar->extradata, *pp, 4);
            }
        }
#endif
        *pp += 4;
        break;
    case 0x0a: /* ISO 639 language descriptor */
        for (i = 0; i + 4 <= desc_len; i += 4) {
            language[i + 0] = get8(pp, desc_end);
            language[i + 1] = get8(pp, desc_end);
            language[i + 2] = get8(pp, desc_end);
            language[i + 3] = ',';
#if 0
        switch (get8(pp, desc_end)) {
            case 0x01: st->disposition |= AV_DISPOSITION_CLEAN_EFFECTS; break;
            case 0x02: st->disposition |= AV_DISPOSITION_HEARING_IMPAIRED; break;
            case 0x03: st->disposition |= AV_DISPOSITION_VISUAL_IMPAIRED; break;
        }
	}
#else
        }
        get8(pp, desc_end);
#endif
        if (i) {
            language[i - 1] = 0;
        }
        break;
    case 0x05: /* registration descriptor */
        dvbci->codec_tag = bytestream_get_le32(pp);
        av_dlog(fc, "reg_desc=%.4s\n", (char*)&dvbci->codec_tag);
        if (item->codec_id == AV_CODEC_ID_NONE &&
            stream_type == STREAM_TYPE_PRIVATE_DATA)
            mpegts_find_stream_type_pmt(item, dvbci->codec_tag, REGD_types);
        break;
#if 0
    case 0x52: /* stream identifier descriptor */
        st->stream_identifier = 1 + get8(pp, desc_end);
        break;
#endif
    case DVB_BROADCAST_ID:
        dvbci->data_id = get16(pp, desc_end);
        break;
    case DVB_CAROUSEL_ID:
        {
            int carId = 0;
            carId = get8(pp, desc_end);
            carId = (carId << 8) | get8(pp, desc_end);
            carId = (carId << 8) | get8(pp, desc_end);
            carId = (carId << 8) | get8(pp, desc_end);
            dvbci->carousel_id = carId;
        }
        break;
    case DVB_DATA_STREAM:
        dvbci->component_tag = get8(pp, desc_end);
        /* Audio and video are sometimes encoded in private streams labelled with
         * a component tag. */
#if 0
         if (item->codec_id == AV_CODEC_ID_NONE &&
             desc_count  == 1 &&
             stream_type == STREAM_TYPE_PRIVATE_DATA)
             mpegts_find_stream_type_pmt(item, dvbci->component_tag,
                                         COMPONENT_TAG_types);
#endif
        break;
    case DVB_VBI_TELETEXT_ID:
        language[0] = get8(pp, desc_end);
        language[1] = get8(pp, desc_end);
        language[2] = get8(pp, desc_end);
        dvbci->txt_type = (get8(pp, desc_end)) >> 3;
        break;
    case DVB_VBI_DATA_ID:
        dvbci->vbi_data = 1; //not parsing the data service descriptors
        break;
    default:
        break;
    }
    *pp = desc_end;
    return 0;
}

static int find_in_list(const int *pids, int pid)
{
    int i;
    for (i=0; i<PMT_PIDS_MAX; i++)
        if (pids[i]==pid)
            return i;
    return -1;
}

static int is_desired_stream(pmt_entry_t *item)
{
    int val = 0;
    switch (item->codec_type)
    {
        case AVMEDIA_TYPE_VIDEO:
        case AVMEDIA_TYPE_AUDIO:
        case AVMEDIA_TYPE_SUBTITLE:
            val = 1;
            break;
        case AVMEDIA_TYPE_DATA:
            switch (item->codec_id)
            {
                case AV_CODEC_ID_DSMCC_B:
                case AV_CODEC_ID_DVB_VBI:
                    val = 1;
                    break;
                default:
                    break;
            }
            break;
        default:
            /* we ignore the other streams */
            break;
    }
    return val;
}

#define HANDLE_PMT_ERROR(MSG) \
    do { av_log(NULL, AV_LOG_ERROR, MSG); return; } while (0)

#define HANDLE_PMT_PARSE_ERROR(PMSG) \
    HANDLE_PMT_ERROR("Something went terribly wrong in PMT parsing" \
                     " when looking at " PMSG "\n")

static void pmt_cb(MpegTSFilter *filter, const uint8_t *section, int section_len)
{
    MpegTSContext *ts = filter->u.section_filter.opaque;
    SectionHeader h1, *h = &h1;

    int last_item = 0;
    int desc_count = 0;
    int streams_changed = 0;
    PESContext *pes;
    const uint8_t *p, *p_end, *desc_list_end;
    int program_info_length, pcr_pid, pid, stream_type;
    int desc_list_len;
    char *language;
    uint32_t prog_reg_desc = 0; /* registration descriptor */

    Mp4Descr mp4_descr[MAX_MP4_DESCR_COUNT] = {{ 0 }};
    int mp4_descr_count = 0;
    int i;

    pmt_entry_t items[PMT_PIDS_MAX];
    memset(&items, 0, sizeof(pmt_entry_t) * PMT_PIDS_MAX);

    // initialize to codec_type_unknown
    for (int i=0; i < PMT_PIDS_MAX; i++)
        items[i].codec_type = AVMEDIA_TYPE_UNKNOWN;

    mpegts_cleanup_streams(ts); /* in case someone else removed streams.. */
 
    av_dlog(ts->stream, "PMT: len %i\n", section_len);
    hex_dump_debug(ts->stream, (uint8_t *)section, section_len);

    p_end = section + section_len - 4;
    p = section;
    if (parse_section_header(h, &p, p_end) < 0)
        HANDLE_PMT_PARSE_ERROR("section header");

    av_dlog(ts->stream, "sid=0x%x sec_num=%d/%d\n",
           h->id, h->sec_num, h->last_sec_num);

    /* Check if this is really a PMT, and if so the right one */
    if (h->tid != PMT_TID)
        HANDLE_PMT_ERROR("pmt_cb() got a TS packet that doesn't have PMT TID\n");

    /* if we require a specific PMT, and this isn't it return silently */
    if (ts->req_sid >= 0 && h->id != ts->req_sid)
    {
#ifdef DEBUG
        av_dlog(ts->stream, "We are looking for program 0x%x, not 0x%x",
               ts->req_sid, h->id);
#endif
         return;
    }

    clear_program(ts, h->id);
    pcr_pid = get16(&p, p_end);
    if (pcr_pid < 0)
        return;
    pcr_pid &= 0x1fff;
    add_pid_to_pmt(ts, h->id, pcr_pid);
    set_pcr_pid(ts->stream, h->id, pcr_pid);

    av_dlog(ts->stream, "pcr_pid=0x%x\n", pcr_pid);

    program_info_length = get16(&p, p_end);
    if (program_info_length < 0)
        return;
    program_info_length &= 0xfff;
    while(program_info_length >= 2) {
        uint8_t tag, len;
        tag = get8(&p, p_end);
        len = get8(&p, p_end);

        av_dlog(ts->stream, "program tag: 0x%02x len=%d\n", tag, len);

        if(len > program_info_length - 2)
            //something else is broken, exit the program_descriptors_loop
            break;
        program_info_length -= len + 2;
        if (tag == 0x1d) { // IOD descriptor
            get8(&p, p_end); // scope
            get8(&p, p_end); // label
            len -= 2;
            mp4_read_iods(ts->stream, p, len, mp4_descr + mp4_descr_count,
                          &mp4_descr_count, MAX_MP4_DESCR_COUNT);
        } else if (tag == 0x05 && len >= 4) { // registration descriptor
            prog_reg_desc = bytestream_get_le32(&p);
            len -= 4;
        }
        p += len;
    }
    p += program_info_length;
    if (p >= p_end)
	return;

    // stop parsing after pmt, we found header
    if (!ts->stream->nb_streams)
        ts->stop_parse = 2;

    for(;;) {
        dvb_caption_info_t dvbci;
        stream_type = get8(&p, p_end);
        if (stream_type < 0)
            break;
        pid = get16(&p, p_end);
        if (pid < 0)
            break;
        pid &= 0x1fff;

        /* break if we are out of space. */
        if (last_item >= PMT_PIDS_MAX) {
            av_log(NULL, AV_LOG_DEBUG,
                   "Could not add new pid 0x%x, i = %i, "
                   "would cause overrun\n", pid, last_item);
            assert(0);
            break;
        }

        items[last_item].pid  = pid;

        mpegts_find_stream_type_pmt(&items[last_item], stream_type, ISO_types);
        if (items[last_item].codec_id == AV_CODEC_ID_NONE) {
            if (prog_reg_desc == AV_RL32("HDMV"))
                mpegts_find_stream_type_pmt(&items[last_item], stream_type, HDMV_types);
            else
                mpegts_find_stream_type_pmt(&items[last_item], stream_type, MISC_types);
        }

        memset(&dvbci, 0, sizeof(dvb_caption_info_t));

        desc_list_len = get16(&p, p_end);
        if (desc_list_len < 0)
            break;
        desc_list_len &= 0xfff;
        desc_list_end = p + desc_list_len;
        if (desc_list_end > p_end)
            break;
        for(;;) {
            if (ff_parse_mpeg2_descriptor(ts->stream, &items[last_item], stream_type, &p, desc_list_end,
                mp4_descr, mp4_descr_count, pid, ts, &dvbci) < 0)
                break;
        }
        p = desc_list_end;

        if (is_desired_stream(&items[last_item])) {
            items[last_item].type = stream_type;
            memcpy(&items[last_item].dvbci, &dvbci,
                   sizeof(dvb_caption_info_t));
            last_item++;
        }

        desc_count++;
    }

    /* if the pmt has changed delete old streams,
     * create new ones, and notify any listener.
     */
    int equal_streams = pmt_equal_streams(ts, items, last_item);
    if (equal_streams != last_item || ts->pid_cnt != last_item)
    {
        AVFormatContext *avctx = ts->stream;
        int idx;
        /* flush out old AVPackets */
        ff_read_frame_flush(avctx);

        /* delete old streams */
        for (idx = ts->pid_cnt-1; idx >= equal_streams; idx--)
            av_remove_stream(ts->stream, ts->pmt_pids[idx], 1);

        /* create new streams */
        for (idx = equal_streams; idx < last_item; idx++)
            mpegts_add_stream(ts, h->id, &items[idx], prog_reg_desc, pcr_pid);

        /* cache pmt */
        void *tmp0 = avctx->cur_pmt_sect;
        void *tmp1 = av_malloc(section_len);
        memcpy(tmp1, section, section_len);
        avctx->cur_pmt_sect = (uint8_t*) tmp1;
        avctx->cur_pmt_sect_len = section_len;
        if (tmp0)
            av_free(tmp0);

        /* notify stream_changed listeners */
        if (avctx->streams_changed)
        {
            av_log(NULL, AV_LOG_DEBUG, "streams_changed()\n");
            avctx->streams_changed(avctx->stream_change_data);
        }
    }

    /* if we are scanning, tell scanner we found the PMT */
    if (ts->scanning)
    {
        ts->pmt_scan_state = PMT_FOUND;
        ts->stop_parse = 1;
    }
}

static int is_pat_same(MpegTSContext *mpegts_ctx,
                       int *pmt_pnums, int *pmt_pids, unsigned int pmt_count)
{
    int idx;
    if (mpegts_ctx->nb_prg != pmt_count)
        return 0;

    for (idx = 0; idx < pmt_count; idx++)
    {
        if ((mpegts_ctx->prg[idx].id  != pmt_pnums[idx]) ||
            (mpegts_ctx->prg[idx].pid != pmt_pids[idx]))
            return 0;
    }
    return 1;
}

// Find number of equal streams in old and new pmt starting at 0
// and stopping at the first different stream.
static int pmt_equal_streams(MpegTSContext *mpegts_ctx,
                             pmt_entry_t* items, int item_cnt)
{
    int limit = mpegts_ctx->pid_cnt < item_cnt ? mpegts_ctx->pid_cnt : item_cnt;
    int idx;

    for (idx = 0; idx < limit; idx++)
    {
        /* check for pid */
        int loc = find_in_list(mpegts_ctx->pmt_pids, items[idx].pid);
        if (loc < 0)
        {
#ifdef DEBUG
            av_log(NULL, AV_LOG_DEBUG,
                   "find_in_list(..,[%d].pid=%d) => -1\n",
                   idx, items[idx].pid);
#endif
            break;
        }

        /* check stream type */
        MpegTSFilter *tss = mpegts_ctx->pids[items[idx].pid];
        if (!tss)
        {
#ifdef DEBUG
            av_log(NULL, AV_LOG_DEBUG,
                   "mpegts_ctx->pids[items[%d].pid=%d] => null\n",
                   idx, items[idx].pid);
#endif
            break;
        }
        if (tss->type == MPEGTS_PES)
        {
            PESContext *pes = (PESContext*) tss->u.pes_filter.opaque;
            if (!pes)
            {
#ifdef DEBUG
                av_log(NULL, AV_LOG_DEBUG, "pes == null, where idx %d\n", idx);
#endif
                break;
            }
            if (pes->stream_type != items[idx].type)
            {
#ifdef DEBUG
                av_log(NULL, AV_LOG_DEBUG,
                       "pes->stream_type != items[%d].type\n", idx);
#endif
                break;
            }
        }
        else if (tss->type == MPEGTS_SECTION)
        {
            SectionContext *sect = (SectionContext*) tss->u.section_filter.opaque;
            if (!sect)
            {
#ifdef DEBUG
                av_log(NULL, AV_LOG_DEBUG, "sect == null, where idx %d\n", idx);
#endif
                break;
            }
            if (sect->stream_type != items[idx].type)
            {
#ifdef DEBUG
                av_log(NULL, AV_LOG_DEBUG,
                       "sect->stream_type != items[%d].type\n", idx);
#endif
                break;
            }
        }
        else
        {
#ifdef DEBUG
            av_log(NULL, AV_LOG_DEBUG,
                   "tss->type != MPEGTS_PES, where idx %d\n", idx);
#endif
            break;
        }
    }
#ifdef DEBUG
    av_log(NULL, AV_LOG_DEBUG, "pmt_equal_streams:%d old:%d new:%d limit:%d\n",
        idx, mpegts_ctx->pid_cnt, item_cnt, limit);
#endif
    return idx;
}

static void mpegts_cleanup_streams(MpegTSContext *ts)
{
    int i;
    int orig_pid_cnt = ts->pid_cnt;
    for (i=0; i<ts->pid_cnt; i++)
    {
        if (!ts->pids[ts->pmt_pids[i]])
        {
            mpegts_remove_stream(ts, ts->pmt_pids[i]);
            i--;
        }
    }
    if (orig_pid_cnt != ts->pid_cnt)
    {
        av_log(NULL, AV_LOG_DEBUG,
               "mpegts_cleanup_streams: pid_cnt bfr %d aft %d\n",
               orig_pid_cnt, ts->pid_cnt);
    }
}

// This was previously in libavutil/internal.h
// Copied here because it is no longer used in the rest of ffmpeg
#define FF_ALLOCZ_OR_GOTO(ctx, p, size, label)\
{\
    p = av_mallocz(size);\
    if (!(p) && (size) != 0) {\
        av_log(ctx, AV_LOG_ERROR, "Cannot allocate memory.\n");\
        goto label;\
    }\
}

static AVStream *new_section_av_stream(SectionContext *sect, enum AVMediaType type,
                                       enum AVCodecID id)
{
    FF_ALLOCZ_OR_GOTO(NULL, sect->st, sizeof(AVStream), fail);

    sect->st = av_new_stream(sect->stream, sect->pid);

    av_set_pts_info(sect->st, 33, 1, 90000);

    sect->st->codecpar->codec_type = type;
    sect->st->codecpar->codec_id   = id;
    sect->st->priv_data = sect;
    sect->st->need_parsing = AVSTREAM_PARSE_NONE;

    return sect->st;
fail: /*for the CHECKED_ALLOCZ macro*/
    return NULL;
}

static void mpegts_add_stream(MpegTSContext *ts, int id, pmt_entry_t* item,
                              uint32_t prog_reg_desc, int pcr_pid)
{
    AVStream *st = NULL;
    int pid = item->pid;

    av_log(NULL, AV_LOG_DEBUG,
           "mpegts_add_stream: at pid 0x%x with type %i\n", item->pid, item->type);

    if (ts->pid_cnt < PMT_PIDS_MAX)
    {
        if (item->type == STREAM_TYPE_DSMCC_B)
        {
            SectionContext *sect = NULL;
            sect = add_section_stream(ts, item->pid, item->type);
            if (!sect)
            {
                av_log(NULL, AV_LOG_ERROR, "mpegts_add_stream: "
                       "error creating Section context for pid 0x%x with type %i\n",
                       item->pid, item->type);
                return;
            }

            st = new_section_av_stream(sect, item->codec_type, item->codec_id);
            if (!st)
            {
                av_log(NULL, AV_LOG_ERROR, "mpegts_add_stream: "
                       "error creating A/V stream for pid 0x%x with type %i\n",
                       item->pid, item->type);
                return;
            }

            st->component_tag = item->dvbci.component_tag;
            st->data_id  = item->dvbci.data_id;
            st->carousel_id = item->dvbci.carousel_id;

            ts->pmt_pids[ts->pid_cnt] = item->pid;
            ts->pid_cnt++;

            av_log(NULL, AV_LOG_DEBUG, "mpegts_add_stream: "
                   "stream #%d, has id 0x%x and codec %s, type %s at 0x%x\n",
                   st->index, st->id, ff_codec_id_string(st->codecpar->codec_id),
                   ff_codec_type_string(st->codecpar->codec_type), st);
        } else {
            PESContext *pes = NULL;

            if (ts->pids[pid] && ts->pids[pid]->type == MPEGTS_PES) {
                pes = ts->pids[pid]->u.pes_filter.opaque;
                st = pes->st;
            } else {
                if (ts->pids[pid]) {
                    //wrongly added sdt filter probably
                    mpegts_close_filter(ts, ts->pids[pid]);
                }
                pes = add_pes_stream(ts, pid, pcr_pid);
                if (pes)
                    st = av_new_stream(pes->stream, pes->pid);
                else
                {
                    av_log(NULL, AV_LOG_ERROR, "mpegts_add_stream: "
                           "error creating PES context for pid 0x%x with type %i\n",
                           item->pid, item->type);
                    return;
                }
            }

            if (!st)
            {
                av_log(NULL, AV_LOG_ERROR, "mpegts_add_stream: "
                       "error creating A/V stream for pid 0x%x with type %i\n",
                       item->pid, item->type);
                return;
            }

            if (!pes->stream_type)
                mpegts_set_stream_info(st, pes, item->type, prog_reg_desc);

            st->codecpar->codec_tag = item->dvbci.codec_tag;

            if (prog_reg_desc == AV_RL32("HDMV") && item->type == 0x83 && pes->sub_st) {
                av_program_add_stream_index(ts->stream, id, pes->sub_st->index);
                pes->sub_st->codecpar->codec_tag = st->codecpar->codec_tag;
            }

            if (st->codecpar->codec_type != item->codec_type ||
                st->codecpar->codec_id   != item->codec_id) {
                st->codecpar->codec_type = item->codec_type;
                st->codecpar->codec_id   = item->codec_id;
            }

            ts->pmt_pids[ts->pid_cnt] = item->pid;
            ts->pid_cnt++;

            if (item->dvbci.language[0])
                av_dict_set(&st->metadata, "language", item->dvbci.language, 0);

            if (item->dvbci.sub_id && (item->codec_id == AV_CODEC_ID_DVB_SUBTITLE))
                st->carousel_id = item->dvbci.sub_id;

            st->component_tag = item->dvbci.component_tag;
            st->disposition   = item->dvbci.disposition;

            av_log(NULL, AV_LOG_DEBUG, "mpegts_add_stream: "
                   "stream #%d, has id 0x%x and codec %s, type %s at 0x%x\n",
                   st->index, st->id, ff_codec_id_string(st->codecpar->codec_id),
                   ff_codec_type_string(st->codecpar->codec_type), st);
        }
        add_pid_to_pmt(ts, id, pid);
        av_program_add_stream_index(ts->stream, id, st->index);
    }
    else
    {
        av_log(NULL, AV_LOG_ERROR,
               "ERROR: adding pes stream at pid 0x%x, pid_cnt = %i\n",
               item->pid, ts->pid_cnt);
    }
}

void mpegts_remove_stream(MpegTSContext *ts, int pid)
{
    av_log(NULL, AV_LOG_DEBUG, "mpegts_remove_stream 0x%x\n", pid);
    if (ts->pids[pid])
    {
        av_log(NULL, AV_LOG_DEBUG, "closing filter for pid 0x%x\n", pid);
        mpegts_close_filter(ts, ts->pids[pid]);
    }
    int indx = find_in_list(ts->pmt_pids, pid);
    if (indx >= 0)
    {
        memmove(ts->pmt_pids+indx, ts->pmt_pids+indx+1, PMT_PIDS_MAX-indx-1);
        ts->pmt_pids[PMT_PIDS_MAX-1] = 0;
        ts->pid_cnt--;
    }
    else
    {
        av_log(NULL, AV_LOG_DEBUG, "ERROR: closing filter for pid 0x%x, indx = %i\n", pid, indx);
    }
}

static void pat_cb(MpegTSFilter *filter, const uint8_t *section, int section_len)
{
    MpegTSContext *ts = filter->u.section_filter.opaque;
    SectionHeader h1, *h = &h1;
    const uint8_t *p, *p_end;
    int sid, pmt_pid;
    AVProgram *program;
    char buf[256];

    int pmt_pnums[PAT_MAX_PMT];
    int pmt_pids[PAT_MAX_PMT];
    unsigned int pmt_count = 0;
    int i;

    av_dlog(ts->stream, "PAT:\n");
    hex_dump_debug(ts->stream, (uint8_t *)section, section_len);

    p_end = section + section_len - 4;
    p = section;
    if (parse_section_header(h, &p, p_end) < 0)
        return;
    if (h->tid != PAT_TID)
        return;

    for (i = 0; i < PAT_MAX_PMT; ++i)
    {
        pmt_pnums[i] = get16(&p, p_end);
        if (pmt_pnums[i] < 0)
            break;

        pmt_pids[i] = get16(&p, p_end) & 0x1fff;
        if (pmt_pids[i] < 0)
            break;

        if (pmt_pids[i] == 0x0)
        {
            av_log(NULL, AV_LOG_ERROR, "Invalid PAT ignored "
                   "MPEG Program Number=0x%x pid=0x%x req_sid=0x%x\n",
                   pmt_pnums[i], pmt_pids[i], ts->req_sid);
            return;
        }

        pmt_count++;

#ifdef DEBUG
        av_log(ts->stream, AV_LOG_DEBUG,
               "MPEG Program Number=0x%x pid=0x%x req_sid=0x%x\n",
               pmt_pnums[i], pmt_pids[i], ts->req_sid);
#endif
    }

    if (!is_pat_same(ts, pmt_pnums, pmt_pids, pmt_count))
    {
#ifdef DEBUG
        av_log(NULL, AV_LOG_DEBUG, "New PAT!\n");
#endif
        /* if there were services, get rid of them */
        ts->nb_prg = 0;

        /* if there are new services, add them */
        for (i = 0; i < pmt_count; ++i)
        {
            snprintf(buf, sizeof(buf), "MPEG Program %x", pmt_pnums[i]);
            add_pat_entry(ts, pmt_pnums[i], pmt_pids[i]);
        }
    }

    int found = 0;
    for (i = 0; i < pmt_count; ++i)
    {
        /* if an MPEG program number is requested, and this is that program,
         * add a filter for the PMT. */
        if (ts->req_sid == pmt_pnums[i])
        {
#ifdef DEBUG
            av_log(NULL, AV_LOG_DEBUG, "Found program number!\n");
#endif
            /* close old filter if it doesn't match */
            if (ts->pmt_filter)
            {
                MpegTSFilter *f = ts->pmt_filter;
                MpegTSSectionFilter *sec = &f->u.section_filter;

                if ((f->pid != pmt_pids[i])     ||
                    (f->type != MPEGTS_SECTION) ||
                    (sec->section_cb != pmt_cb) ||
                    (sec->opaque != ts))
                {
                    mpegts_close_filter(ts, ts->pmt_filter);
                    ts->pmt_filter = NULL;
                }
            }

            /* create new pmt_filter if we need one */
            if (!ts->pmt_filter)
            {
                ts->pmt_filter = mpegts_open_section_filter(
                    ts, pmt_pids[i], pmt_cb, ts, 1);
            }

            found = 1;
        }
    }

    /* if we are scanning for any PAT and not a particular PMT,
     * tell parser it is safe to quit. */
    if (ts->req_sid < 0 && ts->scanning)
    {
#ifdef DEBUG
        av_log(NULL, AV_LOG_DEBUG, "Found PAT, ending scan\n");
#endif
        ts->stop_parse = 1;
    }

    /* if we are looking for a particular MPEG program number,
     * and it is not in this PAT indicate this in "pmt_scan_state"
     * and tell parser it is safe to quit. */ 
    if (ts->req_sid >= 0 && !found)
    {
#ifdef DEBUG
        av_log(NULL, AV_LOG_DEBUG, "Program 0x%x is not in PAT, ending scan\n",
               ts->req_sid);
#endif
        ts->pmt_scan_state = PMT_NOT_IN_PAT;
        ts->stop_parse = 1;
    }
}

static void sdt_cb(MpegTSFilter *filter, const uint8_t *section, int section_len)
{
    MpegTSContext *ts = filter->u.section_filter.opaque;
    SectionHeader h1, *h = &h1;
    const uint8_t *p, *p_end, *desc_list_end, *desc_end;
    int onid, val, sid, desc_list_len, desc_tag, desc_len, service_type;
    char *name, *provider_name;

    av_dlog(ts->stream, "SDT:\n");
    hex_dump_debug(ts->stream, (uint8_t *)section, section_len);

    p_end = section + section_len - 4;
    p = section;
    if (parse_section_header(h, &p, p_end) < 0)
        return;
    if (h->tid != SDT_TID)
        return;
    onid = get16(&p, p_end);
    if (onid < 0)
        return;
    val = get8(&p, p_end);
    if (val < 0)
        return;
    for(;;) {
        sid = get16(&p, p_end);
        if (sid < 0)
            break;
        val = get8(&p, p_end);
        if (val < 0)
            break;
        desc_list_len = get16(&p, p_end);
        if (desc_list_len < 0)
            break;
        desc_list_len &= 0xfff;
        desc_list_end = p + desc_list_len;
        if (desc_list_end > p_end)
            break;
        for(;;) {
            desc_tag = get8(&p, desc_list_end);
            if (desc_tag < 0)
                break;
            desc_len = get8(&p, desc_list_end);
            desc_end = p + desc_len;
            if (desc_end > desc_list_end)
                break;

            av_dlog(ts->stream, "tag: 0x%02x len=%d\n",
                   desc_tag, desc_len);

            switch(desc_tag) {
            case 0x48:
                service_type = get8(&p, p_end);
                if (service_type < 0)
                    break;
                provider_name = getstr8(&p, p_end);
                if (!provider_name)
                    break;
                name = getstr8(&p, p_end);
                if (name) {
                    AVProgram *program = av_new_program(ts->stream, sid);
                    if(program) {
                        av_dict_set(&program->metadata, "service_name", name, 0);
                        av_dict_set(&program->metadata, "service_provider", provider_name, 0);
                    }
                }
                av_free(name);
                av_free(provider_name);
                break;
            default:
                break;
            }
            p = desc_end;
        }
        p = desc_list_end;
    }
}

static SectionContext *add_section_stream(MpegTSContext *ts, int pid, int stream_type)
{
    MpegTSFilter *tss = ts->pids[pid];
    SectionContext *sect = 0;
    if (tss) { /* filter already exists */
        if (tss->type == MPEGTS_SECTION)
            sect = (SectionContext*) tss->u.section_filter.opaque;

        if (sect && (sect->stream_type == stream_type))
            return sect; /* if it's the same stream type, just return ok */

        /* otherwise, kill it, and start a new stream */
        mpegts_close_filter(ts, tss);
    }

    /* create a SECTION context */
    if (!(sect=av_mallocz(sizeof(SectionContext)))) {
        av_log(NULL, AV_LOG_ERROR, "Error: av_mallocz() failed in add_section_stream");
        return 0;
    }
    sect->ts = ts;
    sect->stream = ts->stream;
    sect->pid = pid;
    sect->stream_type = stream_type;
    tss = mpegts_open_section_filter(ts, pid, mpegts_push_section, sect, 1);
    if (!tss) {
        av_free(sect);
        av_log(NULL, AV_LOG_ERROR, "Error: unable to open mpegts Section filter in add_section_stream");
        return 0;
    }

    return sect;
}

/* handle one TS packet */
static int handle_packet(MpegTSContext *ts, const uint8_t *packet)
{
    AVFormatContext *s = ts->stream;
    MpegTSFilter *tss;
    int len, pid, cc, expected_cc, cc_ok, afc, is_start, is_discontinuity,
        has_adaptation, has_payload;
    const uint8_t *p, *p_end;
    int64_t pos;

    pid = AV_RB16(packet + 1) & 0x1fff;

    if (!ts->pids[0]) {
        /* make sure we're always scanning for new PAT's */
        av_log(ts->stream, AV_LOG_INFO, "opening pat filter\n");
        ts->pat_filter = mpegts_open_section_filter(ts, PAT_PID, pat_cb, ts, 1);
    }

    if(pid && discard_pid(ts, pid))
    {
        av_log(ts->stream, AV_LOG_INFO, "discarding pid %d\n", pid);
        return 0;
    }

    is_start = packet[1] & 0x40;
    tss = ts->pids[pid];
    if (ts->auto_guess && tss == NULL && is_start) {
        add_pes_stream(ts, pid, -1);
        tss = ts->pids[pid];
    }
    if (!tss)
        return 0;

    afc = (packet[3] >> 4) & 3;
    if (afc == 0) /* reserved value */
        return 0;
    has_adaptation = afc & 2;
    has_payload = afc & 1;
    is_discontinuity = has_adaptation
                && packet[4] != 0 /* with length > 0 */
                && (packet[5] & 0x80); /* and discontinuity indicated */

    /* continuity check (currently not used) */
    cc = (packet[3] & 0xf);
    expected_cc = has_payload ? (tss->last_cc + 1) & 0x0f : tss->last_cc;
    cc_ok = pid == 0x1FFF // null packet PID
            || is_discontinuity
            || tss->last_cc < 0
            || expected_cc == cc;

    tss->last_cc = cc;
    if (!cc_ok) {
        av_log(ts->stream, AV_LOG_DEBUG,
               "Continuity check failed for pid %d expected %d got %d\n",
               pid, expected_cc, cc);
        if(tss->type == MPEGTS_PES) {
            PESContext *pc = tss->u.pes_filter.opaque;
            pc->flags |= AV_PKT_FLAG_CORRUPT;
        }
    }

    if (!has_payload)
        return 0;
    p = packet + 4;
    if (has_adaptation) {
        /* skip adaptation field */
        p += p[0] + 1;
    }
    /* if past the end of packet, ignore */
    p_end = packet + TS_PACKET_SIZE;
    if (p >= p_end)
        return 0;

    pos = avio_tell(ts->stream->pb);
    ts->pos47= pos % ts->raw_packet_size;

    if (tss->type == MPEGTS_SECTION) {
        if (is_start) {
            /* pointer field present */
            len = *p++;
            if (p + len > p_end)
            {
                av_log(s, AV_LOG_WARNING, "handle_packet: Last section data too long on PID=%#x, %d\n", pid, cc);
                return 0;
            }
            if (len && cc_ok) {
                /* write remaining section bytes */
                write_section_data(s, tss,
                                   p, len, 0);
                /* check whether filter has been closed */
                if (!ts->pids[pid])
                    return 0;
            }
            p += len;
            if (p < p_end) {
                write_section_data(s, tss,
                                   p, p_end - p, 1);
            }
        } else {
            if (cc_ok) {
                write_section_data(s, tss,
                                   p, p_end - p, 0);
            }
        }
    } else {
        int ret;
        // Note: The position here points actually behind the current packet.
        if ((ret = tss->u.pes_filter.pes_cb(tss, p, p_end - p, is_start,
                                            pos - ts->raw_packet_size)) < 0)
            return ret;
    }

    return 0;
}

/* XXX: try to find a better synchro over several packets (use
   get_packet_size() ?) */
static int mpegts_resync(AVFormatContext *s)
{
    AVIOContext *pb = s->pb;
    int c, i;

    for(i = 0;i < MAX_RESYNC_SIZE; i++) {
        c = avio_r8(pb);
        if (avio_feof(pb))
            return -1;
        if (c == 0x47) {
            avio_seek(pb, -1, SEEK_CUR);
            return 0;
        }
    }
    av_log(s, AV_LOG_ERROR, "max resync size reached, could not find sync byte\n");
    /* no sync found */
    return -1;
}

/* return -1 if error or EOF. Return 0 if OK. */
static int read_packet(AVFormatContext *s, uint8_t *buf, int raw_packet_size)
{
    AVIOContext *pb = s->pb;
    int skip, len;

    for(;;) {
        len = avio_read(pb, buf, TS_PACKET_SIZE);
        if (len != TS_PACKET_SIZE)
            return len < 0 ? len : AVERROR_EOF;
        /* check packet sync byte */
        if (buf[0] != 0x47) {
            /* find a new packet start */
            avio_seek(pb, -TS_PACKET_SIZE, SEEK_CUR);
            if (mpegts_resync(s) < 0)
                return AVERROR(EAGAIN);
            else
                continue;
        } else {
            skip = raw_packet_size - TS_PACKET_SIZE;
            if (skip > 0)
                avio_skip(pb, skip);
            break;
        }
    }
    return 0;
}

static int handle_packets(MpegTSContext *ts, int nb_packets)
{
    AVFormatContext *s = ts->stream;
    uint8_t packet[TS_PACKET_SIZE + AV_INPUT_BUFFER_PADDING_SIZE];
    int packet_num, ret = 0;

    if (avio_tell(s->pb) != ts->last_pos) {
        int i;
        av_dlog(ts->stream, "Skipping after seek\n");
        /* seek detected, flush pes buffer */
        for (i = 0; i < NB_PID_MAX; i++) {
            if (ts->pids[i]) {
                if (ts->pids[i]->type == MPEGTS_PES) {
                   PESContext *pes = ts->pids[i]->u.pes_filter.opaque;
                   av_freep(&pes->buffer);
                   pes->data_index = 0;
                   pes->state = MPEGTS_SKIP; /* skip until pes header */
                }
                ts->pids[i]->last_cc = -1;
            }
        }
    }

    ts->stop_parse = 0;
    packet_num = 0;
    memset(packet + TS_PACKET_SIZE, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    for(;;) {
        packet_num++;
        if (nb_packets != 0 && packet_num >= nb_packets ||
            ts->stop_parse > 1) {
            ret = AVERROR(EAGAIN);
            break;
        }
        if (ts->stop_parse > 0)
            break;

        ret = read_packet(s, packet, ts->raw_packet_size);
        if (ret != 0)
            break;
        ret = handle_packet(ts, packet);
        if (ret != 0)
            break;
    }
    ts->last_pos = avio_tell(s->pb);
    return ret;
}

static int mpegts_probe(AVProbeData *p)
{
    const int size= p->buf_size;
    int maxscore=0;
    int sumscore=0;
    int i;
    int check_count= size / TS_FEC_PACKET_SIZE;
#define CHECK_COUNT 10
#define CHECK_BLOCK 100

    if (check_count < CHECK_COUNT)
        return -1;

    for (i=0; i<check_count; i+=CHECK_BLOCK){
        int left = FFMIN(check_count - i, CHECK_BLOCK);
        int score     = analyze(p->buf + TS_PACKET_SIZE     *i, TS_PACKET_SIZE     *left, TS_PACKET_SIZE     , NULL);
        int dvhs_score= analyze(p->buf + TS_DVHS_PACKET_SIZE*i, TS_DVHS_PACKET_SIZE*left, TS_DVHS_PACKET_SIZE, NULL);
        int fec_score = analyze(p->buf + TS_FEC_PACKET_SIZE *i, TS_FEC_PACKET_SIZE *left, TS_FEC_PACKET_SIZE , NULL);
        score = FFMAX3(score, dvhs_score, fec_score);
        sumscore += score;
        maxscore = FFMAX(maxscore, score);
    }

    sumscore = sumscore*CHECK_COUNT/check_count;
    maxscore = maxscore*CHECK_COUNT/CHECK_BLOCK;

    av_dlog(0, "TS score: %d %d\n", sumscore, maxscore);

    if (sumscore > 6)           return AVPROBE_SCORE_MAX + sumscore - CHECK_COUNT;
    else if (maxscore > 6)      return AVPROBE_SCORE_MAX/2 + sumscore - CHECK_COUNT;
    else                        return -1;
}

/* return the 90kHz PCR and the extension for the 27MHz PCR. return
   (-1) if not available */
static int parse_pcr(int64_t *ppcr_high, int *ppcr_low,
                     const uint8_t *packet)
{
    int afc, len, flags;
    const uint8_t *p;
    unsigned int v;

    afc = (packet[3] >> 4) & 3;
    if (afc <= 1)
        return -1;
    p = packet + 4;
    len = p[0];
    p++;
    if (len == 0)
        return -1;
    flags = *p++;
    len--;
    if (!(flags & 0x10))
        return -1;
    if (len < 6)
        return -1;
    v = AV_RB32(p);
    *ppcr_high = ((int64_t)v << 1) | (p[4] >> 7);
    *ppcr_low = ((p[4] & 1) << 8) | p[5];
    return 0;
}

static int mpegts_read_header(AVFormatContext *s)
{
    MpegTSContext *ts = s->priv_data;
    AVIOContext *pb = s->pb;
    uint8_t buf[8*1024] = {0};
    int len, sid, i;
    int64_t pos, probesize =
#if FF_API_PROBESIZE_32
                             s->probesize ? s->probesize : s->probesize2;
#else
                             s->probesize;
#endif

    memset(ts->pids, 0, NB_PID_MAX * sizeof(MpegTSFilter *));

    /* read the first 8192 bytes to get packet size */
    pos = avio_tell(pb);
    len = avio_read(pb, buf, sizeof(buf));
    ts->raw_packet_size = get_packet_size(buf, len);
    av_log(NULL, AV_LOG_DEBUG, "mpegts_read_header: TS packet size = %d\n",
           ts->raw_packet_size);
    if (ts->raw_packet_size <= 0) {
        av_log(s, AV_LOG_WARNING, "Could not detect TS packet size, defaulting to non-FEC/DVHS\n");
        ts->raw_packet_size = TS_PACKET_SIZE;
    }
    ts->stream = s;
    ts->auto_guess = 0;

    if (s->iformat == &ff_mythtv_mpegts_demuxer) {
        /* normal demux */

        if (!ts->auto_guess) {
        /* first do a scan to get all the services */
        /* NOTE: We attempt to seek on non-seekable files as well, as the
         * probe buffer usually is big enough. Only warn if the seek failed
         * on files where the seek should work. */
        if (avio_seek(pb, pos, SEEK_SET) < 0)
            av_log(s, pb->seekable ? AV_LOG_ERROR : AV_LOG_INFO, "Unable to seek back to the start\n");

        /* SDT Scan Removed here. It caused startup delays in TS files
           SDT will not exist in a stripped TS file created by myth. */
#if 0
        mpegts_open_section_filter(ts, SDT_PID, sdt_cb, ts, 1);
#endif

        /* we don't want any PMT pid filters created on first pass */
        ts->req_sid = -1;
 
        ts->scanning = 1;
        ts->pat_filter =
        mpegts_open_section_filter(ts, PAT_PID, pat_cb, ts, 1);

        handle_packets(ts, probesize / ts->raw_packet_size);
        ts->scanning = 0;

        if (ts->nb_prg <= 0) {
            /* Guess this is a raw transport stream with no PAT tables. */
            ts->auto_guess = 1;
            s->ctx_flags |= AVFMTCTX_NOHEADER;
               goto do_pcr;
        }

        ts->scanning = 1;
        ts->pmt_scan_state = PMT_NOT_YET_FOUND;
        /* tune to first service found */
        for (i = 0; ((i < ts->nb_prg) &&
                     (ts->pmt_scan_state == PMT_NOT_YET_FOUND)); i++)
        {
#ifdef DEBUG
            av_log(ts->stream, AV_LOG_DEBUG, "Tuning to pnum: 0x%x\n",
                   ts->prg[i].id);
#endif
            
            /* now find the info for the first service if we found any,
               otherwise try to filter all PATs */
            
            avio_seek(pb, pos, SEEK_SET);
            ts->req_sid = sid = ts->prg[i].id;
            handle_packets(ts, probesize / ts->raw_packet_size);

            /* fallback code to deal with broken streams from
             * DBOX2/Firewire cable boxes. */
            if (ts->pmt_filter &&
                (ts->pmt_scan_state == PMT_NOT_YET_FOUND))
            {
                av_log(NULL, AV_LOG_ERROR,
                       "Tuning to pnum: 0x%x without CRC check on PMT\n",
                       ts->prg[i].id);
                /* turn off crc checking */
                ts->pmt_filter->u.section_filter.check_crc = 0;
                /* try again */
                avio_seek(pb, pos, SEEK_SET);
                ts->req_sid = sid = ts->prg[i].id;
                handle_packets(ts, probesize / ts->raw_packet_size);
            }

            /* fallback code to deal with streams that are not complete PMT
             * streams (BBC iPlayer IPTV as an example) */
            if (ts->pmt_filter &&
                (ts->pmt_scan_state == PMT_NOT_YET_FOUND))
            {
                av_log(NULL, AV_LOG_ERROR,
                       "Overriding PMT data length, using "
                       "contents of first TS packet only!\n");
                ts->pmt_filter->pmt_chop_at_ts = 1;
                /* try again */
                avio_seek(pb, pos, SEEK_SET);
                ts->req_sid = sid = ts->prg[i].id;
                handle_packets(ts, probesize / ts->raw_packet_size);
            }
        }
        ts->scanning = 0;

        /* if we could not find any PMTs, fail */
        if (ts->pmt_scan_state == PMT_NOT_YET_FOUND)
        {
            av_log(NULL, AV_LOG_ERROR,
                   "mpegts_read_header: could not find any PMT's\n");
            goto fail;
        }
        av_dlog(ts->stream, "tuning done\n");
        }

        s->ctx_flags |= AVFMTCTX_NOHEADER;
    } else {
        AVStream *st;
        int pcr_pid, pid, nb_packets, nb_pcrs, ret, pcr_l;
        int64_t pcrs[2], pcr_h;
        int packet_count[2];
        uint8_t packet[TS_PACKET_SIZE];

        /* only read packets */

    do_pcr:
        st = avformat_new_stream(s, NULL);
        if (!st)
        {
            av_log(NULL, AV_LOG_ERROR, "mpegts_read_header: "
                   "av_new_stream() failed\n");
            goto fail;
        }
        avpriv_set_pts_info(st, 60, 1, 27000000);
        st->codecpar->codec_type = AVMEDIA_TYPE_DATA;
        st->codecpar->codec_id = AV_CODEC_ID_MPEG2TS;

        /* we iterate until we find two PCRs to estimate the bitrate */
        pcr_pid = -1;
        nb_pcrs = 0;
        nb_packets = 0;
        for(;;) {
            ret = read_packet(s, packet, ts->raw_packet_size);
            if (ret < 0)
            {
                av_log(NULL, AV_LOG_ERROR, "mpegts_read_header: "
                       "read_packet() failed\n");
                return -1;
            }
            pid = AV_RB16(packet + 1) & 0x1fff;
            if ((pcr_pid == -1 || pcr_pid == pid) &&
                parse_pcr(&pcr_h, &pcr_l, packet) == 0) {
                pcr_pid = pid;
                packet_count[nb_pcrs] = nb_packets;
                pcrs[nb_pcrs] = pcr_h * 300 + pcr_l;
                nb_pcrs++;
                if (nb_pcrs >= 2)
                    break;
            }
            nb_packets++;
        }

        /* NOTE1: the bitrate is computed without the FEC */
        /* NOTE2: it is only the bitrate of the start of the stream */
        ts->pcr_incr = (pcrs[1] - pcrs[0]) / (packet_count[1] - packet_count[0]);
        ts->cur_pcr = pcrs[0] - ts->pcr_incr * packet_count[0];
        s->bit_rate = (TS_PACKET_SIZE * 8) * 27e6 / ts->pcr_incr;
        st->codecpar->bit_rate = s->bit_rate;
        st->start_time = ts->cur_pcr;
        av_dlog(ts->stream, "start=%0.3f pcr=%0.3f incr=%d\n",
                st->start_time / 1000000.0, pcrs[0] / 27e6, ts->pcr_incr);
    }

    avio_seek(pb, pos, SEEK_SET);
    return 0;
 fail:
    return -1;
}

#define MAX_PACKET_READAHEAD ((128 * 1024) / 188)

static int mpegts_raw_read_packet(AVFormatContext *s,
                                  AVPacket *pkt)
{
    MpegTSContext *ts = s->priv_data;
    int ret, i;
    int64_t pcr_h, next_pcr_h, pos;
    int pcr_l, next_pcr_l;
    uint8_t pcr_buf[12];

    if (av_new_packet(pkt, TS_PACKET_SIZE) < 0)
        return AVERROR(ENOMEM);
    pkt->pos= avio_tell(s->pb);
    ret = read_packet(s, pkt->data, ts->raw_packet_size);
    if (ret < 0) {
        av_free_packet(pkt);
        return ret;
    }
    if (ts->mpeg2ts_compute_pcr) {
        /* compute exact PCR for each packet */
        if (parse_pcr(&pcr_h, &pcr_l, pkt->data) == 0) {
            /* we read the next PCR (XXX: optimize it by using a bigger buffer */
            pos = avio_tell(s->pb);
            for(i = 0; i < MAX_PACKET_READAHEAD; i++) {
                avio_seek(s->pb, pos + i * ts->raw_packet_size, SEEK_SET);
                avio_read(s->pb, pcr_buf, 12);
                if (parse_pcr(&next_pcr_h, &next_pcr_l, pcr_buf) == 0) {
                    /* XXX: not precise enough */
                    ts->pcr_incr = ((next_pcr_h - pcr_h) * 300 + (next_pcr_l - pcr_l)) /
                        (i + 1);
                    break;
                }
            }
            avio_seek(s->pb, pos, SEEK_SET);
            /* no next PCR found: we use previous increment */
            ts->cur_pcr = pcr_h * 300 + pcr_l;
        }
        pkt->pts = ts->cur_pcr;
        pkt->duration = ts->pcr_incr;
        ts->cur_pcr += ts->pcr_incr;
    }
    pkt->stream_index = 0;
    return 0;
}

static int mpegts_read_packet(AVFormatContext *s,
                              AVPacket *pkt)
{
    MpegTSContext *ts = s->priv_data;
    int ret, i;

    ts->pkt = pkt;
    ret = handle_packets(ts, 0);
    if (ret < 0) {
        av_free_packet(ts->pkt);
        /* flush pes data left */
        for (i = 0; i < NB_PID_MAX; i++) {
            if (ts->pids[i] && ts->pids[i]->type == MPEGTS_PES) {
                PESContext *pes = ts->pids[i]->u.pes_filter.opaque;
                if (pes->state == MPEGTS_PAYLOAD && pes->data_index > 0) {
                    new_pes_packet(pes, pkt);
                    pes->state = MPEGTS_SKIP;
                    ret = 0;
                    break;
                }
            }
        }
    }

    return ret;
}

static int mpegts_read_close(AVFormatContext *s)
{
    MpegTSContext *ts = s->priv_data;
    int i;

    clear_programs(ts);

    for(i=0;i<NB_PID_MAX;i++)
        if (ts->pids[i]) mpegts_close_filter(ts, ts->pids[i]);

    return 0;
}

static int64_t mpegts_get_pcr(AVFormatContext *s, int stream_index,
                              int64_t *ppos, int64_t pos_limit)
{
    MpegTSContext *ts = s->priv_data;
    int64_t pos, timestamp;
    uint8_t buf[TS_PACKET_SIZE];
    int pcr_l, pcr_pid = ((PESContext*)s->streams[stream_index]->priv_data)->pcr_pid;
    pos = ((*ppos  + ts->raw_packet_size - 1 - ts->pos47) / ts->raw_packet_size) * ts->raw_packet_size + ts->pos47;
    while(pos < pos_limit) {
        if (avio_seek(s->pb, pos, SEEK_SET) < 0)
            return AV_NOPTS_VALUE;
        if (avio_read(s->pb, buf, TS_PACKET_SIZE) != TS_PACKET_SIZE)
            return AV_NOPTS_VALUE;
        if (buf[0] != 0x47) {
            if (mpegts_resync(s) < 0)
                return AV_NOPTS_VALUE;
            pos = avio_tell(s->pb);
            continue;
        }
        if ((pcr_pid < 0 || (AV_RB16(buf + 1) & 0x1fff) == pcr_pid) &&
            parse_pcr(&timestamp, &pcr_l, buf) == 0) {
            *ppos = pos;
            return timestamp;
        }
        pos += ts->raw_packet_size;
    }

    return AV_NOPTS_VALUE;
}

static int64_t mpegts_get_dts(AVFormatContext *s, int stream_index,
                              int64_t *ppos, int64_t pos_limit)
{
    MpegTSContext *ts = s->priv_data;
    int64_t pos;
    pos = ((*ppos  + ts->raw_packet_size - 1 - ts->pos47) / ts->raw_packet_size) * ts->raw_packet_size + ts->pos47;
    ff_read_frame_flush(s);
    if (avio_seek(s->pb, pos, SEEK_SET) < 0)
        return AV_NOPTS_VALUE;
    while(pos < pos_limit) {
        int ret;
        AVPacket pkt;
        av_init_packet(&pkt);
        ret= av_read_frame(s, &pkt);
        if(ret < 0)
            return AV_NOPTS_VALUE;
        av_free_packet(&pkt);
        if(pkt.dts != AV_NOPTS_VALUE && pkt.pos >= 0){
            ff_reduce_index(s, pkt.stream_index);
            av_add_index_entry(s->streams[pkt.stream_index], pkt.pos, pkt.dts, 0, 0, AVINDEX_KEYFRAME /* FIXME keyframe? */);
            if(pkt.stream_index == stream_index){
                *ppos= pkt.pos;
                return pkt.dts;
            }
        }
        pos = pkt.pos;
    }

    return AV_NOPTS_VALUE;
}

#ifdef USE_SYNCPOINT_SEARCH

static int read_seek2(AVFormatContext *s,
                      int stream_index,
                      int64_t min_ts,
                      int64_t target_ts,
                      int64_t max_ts,
                      int flags)
{
    int64_t pos;

    int64_t ts_ret, ts_adj;
    int stream_index_gen_search;
    AVStream *st;
    AVParserState *backup;

    backup = ff_store_parser_state(s);

    // detect direction of seeking for search purposes
    flags |= (target_ts - min_ts > (uint64_t)(max_ts - target_ts)) ?
             AVSEEK_FLAG_BACKWARD : 0;

    if (flags & AVSEEK_FLAG_BYTE) {
        // use position directly, we will search starting from it
        pos = target_ts;
    } else {
        // search for some position with good timestamp match
        if (stream_index < 0) {
            stream_index_gen_search = av_find_default_stream_index(s);
            if (stream_index_gen_search < 0) {
                ff_restore_parser_state(s, backup);
                return -1;
            }

            st = s->streams[stream_index_gen_search];
            // timestamp for default must be expressed in AV_TIME_BASE units
            ts_adj = av_rescale(target_ts,
                                st->time_base.den,
                                AV_TIME_BASE * (int64_t)st->time_base.num);
        } else {
            ts_adj = target_ts;
            stream_index_gen_search = stream_index;
        }
        pos = ff_gen_search(s, stream_index_gen_search, ts_adj,
                            0, INT64_MAX, -1,
                            AV_NOPTS_VALUE,
                            AV_NOPTS_VALUE,
                            flags, &ts_ret, mpegts_get_pcr);
        if (pos < 0) {
            ff_restore_parser_state(s, backup);
            return -1;
        }
    }

    // search for actual matching keyframe/starting position for all streams
    if (ff_gen_syncpoint_search(s, stream_index, pos,
                                min_ts, target_ts, max_ts,
                                flags) < 0) {
        ff_restore_parser_state(s, backup);
        return -1;
    }

    ff_free_parser_state(s, backup);
    return 0;
}

static int read_seek(AVFormatContext *s, int stream_index, int64_t target_ts, int flags)
{
    int ret;
    if (flags & AVSEEK_FLAG_BACKWARD) {
        flags &= ~AVSEEK_FLAG_BACKWARD;
        ret = read_seek2(s, stream_index, INT64_MIN, target_ts, target_ts, flags);
        if (ret < 0)
            // for compatibility reasons, seek to the best-fitting timestamp
            ret = read_seek2(s, stream_index, INT64_MIN, target_ts, INT64_MAX, flags);
    } else {
        ret = read_seek2(s, stream_index, target_ts, target_ts, INT64_MAX, flags);
        if (ret < 0)
            // for compatibility reasons, seek to the best-fitting timestamp
            ret = read_seek2(s, stream_index, INT64_MIN, target_ts, INT64_MAX, flags);
    }
    return ret;
}

#endif

/**************************************************************/
/* parsing functions - called from other demuxers such as RTP */

MpegTSContext *avpriv_mpegts_parse_open(AVFormatContext *s)
{
    MpegTSContext *ts;

    ts = av_mallocz(sizeof(MpegTSContext));
    if (!ts)
        return NULL;
    /* no stream case, currently used by RTP */
    ts->raw_packet_size = TS_PACKET_SIZE;
    ts->stream = s;
    ts->auto_guess = 1;
    mpegts_open_section_filter(ts, SDT_PID, sdt_cb, ts, 1);
    mpegts_open_section_filter(ts, PAT_PID, pat_cb, ts, 1);

    return ts;
}

/* return the consumed length if a packet was output, or -1 if no
   packet is output */
int avpriv_mpegts_parse_packet(MpegTSContext *ts, AVPacket *pkt,
                               const uint8_t *buf, int len)
{
    int len1;

    len1 = len;
    ts->pkt = pkt;
    for(;;) {
        ts->stop_parse = 0;
        if (len < TS_PACKET_SIZE)
            return -1;
        if (buf[0] != 0x47) {
            buf++;
            len--;
        } else {
            handle_packet(ts, buf);
            buf += TS_PACKET_SIZE;
            len -= TS_PACKET_SIZE;
            if (ts->stop_parse == 1)
                break;
        }
    }
    return len1 - len;
}

void avpriv_mpegts_parse_close(MpegTSContext *ts)
{
    int i;

    for(i=0;i<NB_PID_MAX;i++)
        av_free(ts->pids[i]);
    av_free(ts);
}

AVStream *av_new_stream(AVFormatContext *s, int id)
{
    AVStream *st = avformat_new_stream(s, NULL);
    if (st)
        st->id = id;
    return st;
}

void av_set_pts_info(AVStream *s, int pts_wrap_bits,
                     unsigned int pts_num, unsigned int pts_den)
{
    avpriv_set_pts_info(s, pts_wrap_bits, pts_num, pts_den);
}

AVInputFormat ff_mythtv_mpegts_demuxer = {
    .name           = "mpegts",
    .long_name      = NULL_IF_CONFIG_SMALL("MPEG-2 transport stream format"),
    .priv_data_size = sizeof(MpegTSContext),
    .read_probe     = mpegts_probe,
    .read_header    = mpegts_read_header,
    .read_packet    = mpegts_read_packet,
    .read_close     = mpegts_read_close,
    .read_timestamp = mpegts_get_dts,
    .flags = AVFMT_SHOW_IDS|AVFMT_TS_DISCONT,
#ifdef USE_SYNCPOINT_SEARCH
    .read_seek2 = read_seek2,
#endif
};

AVInputFormat ff_mythtv_mpegtsraw_demuxer = {
    .name           = "mpegtsraw",
    .long_name      = NULL_IF_CONFIG_SMALL("MPEG-2 raw transport stream format"),
    .priv_data_size = sizeof(MpegTSContext),
    .read_header    = mpegts_read_header,
    .read_packet    = mpegts_raw_read_packet,
    .read_close     = mpegts_read_close,
    .read_timestamp = mpegts_get_dts,
    .flags = AVFMT_SHOW_IDS|AVFMT_TS_DISCONT,
#ifdef USE_SYNCPOINT_SEARCH
    .read_seek2 = read_seek2,
#endif
    .priv_class = &mpegtsraw_class,
};
