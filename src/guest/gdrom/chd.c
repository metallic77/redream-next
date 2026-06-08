#include "guest/gdrom/chd.h"
#include "core/assert.h"
#include "core/log.h"
#include "guest/gdrom/disc.h"
#include "guest/gdrom/gdrom_types.h"

#include <libchdr/chd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* CHD stores each sector as 2352 bytes of CD data + 96 bytes of subcode.
   We only use the 2352 byte part. */
#define CHD_SECTOR_RAW  2352
#define CHD_SECTOR_SUB  96
#define CHD_FRAME_SIZE  (CHD_SECTOR_RAW + CHD_SECTOR_SUB)

/* GD-ROM discs have exactly this many frames */
#define GDROM_TOTAL_FRAMES 549300

/* tracks are padded to multiples of this in the CHD hunk stream */
#define CHD_TRACK_PADDING 4

/* gap between session 1 lead-out and session 2 lead-in on MIL-CDs */
#define SESSION_GAP 11400

struct chd_state {
  struct disc disc;   /* must be first — we cast disc* <-> chd_state* */

  chd_file *chd;
  uint8_t  *hunk_buf;
  uint32_t  hunkbytes;
  uint32_t  sph;        /* sectors per hunk */
  uint32_t  cached_hunk;

  struct session sessions[DISC_MAX_SESSIONS];
  int        num_sessions;
  struct track tracks[DISC_MAX_TRACKS];
  int        num_tracks;

  /* per-track: byte offset from start of hunk stream to FAD 0 */
  int32_t track_offsets[DISC_MAX_TRACKS];
  /* per-track: raw sector size stored in the CHD (2048, 2336 or 2352) */
  int     track_sector_size[DISC_MAX_TRACKS];
  /* per-track: is audio byteswapped (CHDv5 GD-ROM audio) */
  int     track_swap[DISC_MAX_TRACKS];
};

/* -------------------------------------------------------------------------
   internal helpers
   ---------------------------------------------------------------------- */

static int chd_load_hunk(struct chd_state *s, uint32_t hunk) {
  if (s->cached_hunk == hunk) {
    return 1;
  }
  if (chd_read(s->chd, hunk, s->hunk_buf) != CHDERR_NONE) {
    LOG_WARNING("chd: failed to read hunk %u", hunk);
    return 0;
  }
  s->cached_hunk = hunk;
  return 1;
}

static int sector_size_from_type(const char *type) {
  if (strcmp(type, "AUDIO")        == 0) return CHD_SECTOR_RAW;  /* 2352 */
  if (strcmp(type, "MODE1")        == 0) return 2048;
  if (strcmp(type, "MODE1/2048")   == 0) return 2048;
  if (strcmp(type, "MODE1_RAW")    == 0) return CHD_SECTOR_RAW;
  if (strcmp(type, "MODE1/2352")   == 0) return CHD_SECTOR_RAW;
  if (strcmp(type, "MODE2")        == 0) return 2336;
  if (strcmp(type, "MODE2/2336")   == 0) return 2336;
  if (strcmp(type, "MODE2_RAW")    == 0) return CHD_SECTOR_RAW;
  if (strcmp(type, "MODE2/2352")   == 0) return CHD_SECTOR_RAW;
  if (strcmp(type, "CDI/2352")     == 0) return CHD_SECTOR_RAW;
  LOG_WARNING("chd: unknown track type '%s', assuming 2352", type);
  return CHD_SECTOR_RAW;
}

static int ctrl_from_type(const char *type) {
  return (strcmp(type, "AUDIO") == 0) ? 0 : 4;
}

/* Map CHD stored sector size → redream sector format */
static int sector_fmt_from_size(int stored_size, int ctrl) {
  if (ctrl == 0)      return GD_SECTOR_CDDA;
  if (stored_size == 2048) return GD_SECTOR_M1;
  if (stored_size == 2336) return GD_SECTOR_M2;
  return GD_SECTOR_M1;  /* 2352 raw — treat as Mode1 raw */
}

/* -------------------------------------------------------------------------
   disc vtable implementation
   ---------------------------------------------------------------------- */

static void chd_disc_destroy(struct disc *disc) {
  struct chd_state *s = (struct chd_state *)disc;
  if (s->chd)     chd_close(s->chd);
  if (s->hunk_buf) free(s->hunk_buf);
  free(s);
}

static int chd_disc_get_format(struct disc *disc) {
  struct chd_state *s = (struct chd_state *)disc;
  /* if it has more than one session it's a GD-ROM, otherwise plain CD */
  return (s->num_sessions == 2) ? GD_DISC_GDROM : GD_DISC_CDROM_XA;
}

static int chd_disc_get_num_sessions(struct disc *disc) {
  struct chd_state *s = (struct chd_state *)disc;
  return s->num_sessions;
}

static struct session *chd_disc_get_session(struct disc *disc, int n) {
  struct chd_state *s = (struct chd_state *)disc;
  CHECK_LT(n, s->num_sessions);
  return &s->sessions[n];
}

static int chd_disc_get_num_tracks(struct disc *disc) {
  struct chd_state *s = (struct chd_state *)disc;
  return s->num_tracks;
}

static struct track *chd_disc_get_track(struct disc *disc, int n) {
  struct chd_state *s = (struct chd_state *)disc;
  CHECK_LT(n, s->num_tracks);
  return &s->tracks[n];
}

static void chd_disc_get_toc(struct disc *disc, int area,
                              struct track **first_track,
                              struct track **last_track,
                              int *leadin_fad, int *leadout_fad) {
  struct chd_state *s = (struct chd_state *)disc;
  CHECK_LT(area, s->num_sessions);
  struct session *ses = &s->sessions[area];
  *first_track  = &s->tracks[ses->first_track];
  *last_track   = &s->tracks[ses->last_track];
  *leadin_fad   = ses->leadin_fad;
  *leadout_fad  = ses->leadout_fad;
}

static int chd_disc_read_sector(struct disc *disc, int fad, int sector_fmt,
                                 int sector_mask, void *dst) {
  struct chd_state *s = (struct chd_state *)disc;

  CHECK(sector_fmt == GD_SECTOR_ANY || sector_fmt == GD_SECTOR_M1 ||
        sector_fmt == GD_SECTOR_M2  || sector_fmt == GD_SECTOR_CDDA);
  CHECK(sector_mask == GD_MASK_DATA);

  struct track *track = disc_lookup_track(disc, fad);
  CHECK_NOTNULL(track);

  int ti = (int)(track - s->tracks);
  int stored_size = s->track_sector_size[ti];

  /* position within the hunk stream */
  int32_t fad_in_stream = fad + s->track_offsets[ti];
  CHECK_GE(fad_in_stream, 0);

  uint32_t hunk      = (uint32_t)fad_in_stream / s->sph;
  uint32_t hunk_ofs  = (uint32_t)fad_in_stream % s->sph;

  if (!chd_load_hunk(s, hunk)) {
    return 0;
  }

  /* pointer to the start of this sector's raw bytes in the hunk buffer */
  const uint8_t *raw = s->hunk_buf + hunk_ofs * CHD_FRAME_SIZE;

  if (stored_size == CHD_SECTOR_RAW) {
    /* raw 2352: skip the 16-byte sector header to reach the 2048-byte payload,
       unless it's audio in which case copy the full 2352 bytes */
    if (track->sector_fmt == GD_SECTOR_CDDA) {
      memcpy(dst, raw, CHD_SECTOR_RAW);
      /* byteswap audio if needed (CHDv5 GD-ROM audio is big-endian PCM) */
      if (s->track_swap[ti]) {
        uint8_t *d = (uint8_t *)dst;
        for (int i = 0; i < CHD_SECTOR_RAW; i += 2) {
          uint8_t t = d[i]; d[i] = d[i+1]; d[i+1] = t;
        }
      }
    } else {
      /* skip 16-byte header → 2048 bytes of user data */
      memcpy(dst, raw + 16, 2048);
    }
    return 2048;
  } else if (stored_size == 2336) {
    /* Mode2 cooked: 8-byte sub-header precedes the 2048-byte payload */
    memcpy(dst, raw + 8, 2048);
    return 2048;
  } else {
    /* 2048 cooked: user data only, copy directly */
    memcpy(dst, raw, 2048);
    return 2048;
  }
}

/* -------------------------------------------------------------------------
   GD-ROM session layout (same as gdi.c)
   ---------------------------------------------------------------------- */

static void chd_fill_gdrom_sessions(struct chd_state *s) {
  /* session 0 — single density area: tracks 0 and 1 */
  s->sessions[0].leadin_fad   = 0x0000;
  s->sessions[0].leadout_fad  = 0x4650;
  s->sessions[0].first_track  = 0;
  s->sessions[0].last_track   = 1;

  /* session 1 — high density area: tracks 2+ */
  s->sessions[1].leadin_fad   = 0xb05e;
  s->sessions[1].leadout_fad  = 0x861b4;
  s->sessions[1].first_track  = 2;
  s->sessions[1].last_track   = s->num_tracks - 1;

  s->num_sessions = 2;
}

/* -------------------------------------------------------------------------
   CHD metadata parsing
   ---------------------------------------------------------------------- */

static int chd_parse(struct chd_state *s) {
  const chd_header *head = chd_get_header(s->chd);

  s->hunkbytes    = head->hunkbytes;
  s->hunk_buf     = (uint8_t *)malloc(s->hunkbytes);
  CHECK_NOTNULL(s->hunk_buf);
  s->cached_hunk  = 0xFFFFFFFF;
  s->sph          = s->hunkbytes / CHD_FRAME_SIZE;

  if (s->hunkbytes % CHD_FRAME_SIZE != 0) {
    LOG_WARNING("chd: hunkbytes %u is not a multiple of %u",
                s->hunkbytes, CHD_FRAME_SIZE);
    return 0;
  }

  char   meta[512];
  uint32_t meta_len, tag;
  uint8_t  flags;
  uint32_t total_frames  = 150;   /* 2-second pregap at start */
  uint32_t stream_offset = 0;     /* running offset into hunk stream in frames */
  int      is_gdrom      = (head->version < 5); /* CHD <v5 was GD-ROM only */
  int      need_audio_swap = 0;

  for (;;) {
    char type[16], subtype[16], pgtype[16], pgsub[16];
    int tkid = -1, frames = 0, pregap = 0, postgap = 0, padframes = 0;
    chd_error err;

    /* Try metadata tags in flycast's priority order:
       CHT2 (CDROM v2) → CHTR (CDROM v1) → CHGD (GD-ROM) */
    err = chd_get_metadata(s->chd, CDROM_TRACK_METADATA2_TAG,
                           (uint32_t)s->num_tracks, meta, sizeof(meta),
                           &meta_len, &tag, &flags);
    if (err == CHDERR_NONE) {
      sscanf(meta, CDROM_TRACK_METADATA2_FORMAT,
             &tkid, type, subtype, &frames, &pregap, pgtype, pgsub, &postgap);
    } else {
      err = chd_get_metadata(s->chd, CDROM_TRACK_METADATA_TAG,
                             (uint32_t)s->num_tracks, meta, sizeof(meta),
                             &meta_len, &tag, &flags);
      if (err == CHDERR_NONE) {
        sscanf(meta, CDROM_TRACK_METADATA_FORMAT,
               &tkid, type, subtype, &frames);
      } else {
        /* try old GD-ROM tag first, then new GD-ROM tag */
        err = chd_get_metadata(s->chd, GDROM_OLD_METADATA_TAG,
                               (uint32_t)s->num_tracks, meta, sizeof(meta),
                               &meta_len, &tag, &flags);
        if (err != CHDERR_NONE) {
          err = chd_get_metadata(s->chd, GDROM_TRACK_METADATA_TAG,
                                 (uint32_t)s->num_tracks, meta, sizeof(meta),
                                 &meta_len, &tag, &flags);
          if (err == CHDERR_NONE) {
            need_audio_swap = 1;
          }
        }
        if (err != CHDERR_NONE) {
          break;  /* no more tracks */
        }
        sscanf(meta, GDROM_TRACK_METADATA_FORMAT,
               &tkid, type, subtype, &frames, &padframes,
               &pregap, pgtype, pgsub, &postgap);
        is_gdrom = 1;
      }
    }

    if (tkid != s->num_tracks + 1) {
      LOG_WARNING("chd: unexpected track id %d (expected %d)", tkid,
                  s->num_tracks + 1);
      return 0;
    }

    if (s->num_tracks >= DISC_MAX_TRACKS) {
      LOG_WARNING("chd: too many tracks (max %d)", DISC_MAX_TRACKS);
      return 0;
    }

    int stored_size = sector_size_from_type(type);
    int ctrl        = ctrl_from_type(type);
    int ti          = s->num_tracks;

    struct track *track = &s->tracks[ti];
    track->num          = ti + 1;
    track->fad          = (int)total_frames;
    track->ctrl         = ctrl;
    track->sector_fmt   = sector_fmt_from_size(stored_size, ctrl);
    track->sector_size  = stored_size;
    track->filename[0]  = '\0';
    track->file_offset  = 0;

    /* offset into the hunk stream: subtract the track's starting FAD so that
       (fad + track_offset) gives us the frame index within the stream */
    s->track_offsets[ti]     = (int32_t)stream_offset - (int32_t)total_frames;
    s->track_sector_size[ti] = stored_size;
    s->track_swap[ti]        = (!ctrl && need_audio_swap) ? 1 : 0;

    LOG_INFO("chd: track %d type=%s fad=%d frames=%d stored_size=%d",
             ti + 1, type, track->fad, frames, stored_size);

    total_frames  += (uint32_t)frames;

    /* advance stream offset by padded frame count */
    int padded     = (frames + CHD_TRACK_PADDING - 1) / CHD_TRACK_PADDING;
    stream_offset += (uint32_t)(padded * CHD_TRACK_PADDING);

    s->num_tracks++;
  }

  if (s->num_tracks == 0) {
    LOG_WARNING("chd: no tracks found");
    return 0;
  }

  if (is_gdrom) {
    if (total_frames != GDROM_TOTAL_FRAMES) {
      LOG_WARNING("chd: GD-ROM total frames %u (expected %u) in %d tracks",
                  total_frames, GDROM_TOTAL_FRAMES, s->num_tracks);
    }
    if (s->num_tracks < 3) {
      LOG_WARNING("chd: GD-ROM needs at least 3 tracks, got %d", s->num_tracks);
      return 0;
    }
    chd_fill_gdrom_sessions(s);
  } else {
    /* plain CD-ROM: single session */
    s->sessions[0].first_track = 0;
    s->sessions[0].last_track  = s->num_tracks - 1;
    s->sessions[0].leadin_fad  = s->tracks[0].fad;

    if (s->num_tracks > 1) {
      /* MIL-CD: two sessions, apply the gap to the last track */
      s->tracks[s->num_tracks - 1].fad        += SESSION_GAP;
      s->track_offsets[s->num_tracks - 1]     -= SESSION_GAP;
      s->sessions[1].first_track = s->num_tracks - 1;
      s->sessions[1].last_track  = s->num_tracks - 1;
      s->sessions[1].leadin_fad  = s->tracks[s->num_tracks - 1].fad;
      s->sessions[0].leadout_fad = s->sessions[1].leadin_fad;
      s->sessions[1].leadout_fad = (int)(total_frames + SESSION_GAP - 1);
      s->num_sessions = 2;
    } else {
      s->sessions[0].leadout_fad = (int)(total_frames - 1);
      s->num_sessions = 1;
    }
  }

  return 1;
}

/* -------------------------------------------------------------------------
   public constructor
   ---------------------------------------------------------------------- */

struct disc *chd_create(const char *filename) {
  struct chd_state *s = (struct chd_state *)calloc(1, sizeof(struct chd_state));
  if (!s) return NULL;

  /* open the CHD file */
  chd_error err = chd_open(filename, CHD_OPEN_READ, NULL, &s->chd);
  if (err != CHDERR_NONE) {
    LOG_WARNING("chd: failed to open '%s' (error %d)", filename, err);
    free(s);
    return NULL;
  }

  LOG_INFO("chd: opening '%s'", filename);

  if (!chd_parse(s)) {
    chd_disc_destroy((struct disc *)s);
    return NULL;
  }

  /* wire up the vtable */
  s->disc.destroy          = &chd_disc_destroy;
  s->disc.get_format       = &chd_disc_get_format;
  s->disc.get_num_sessions = &chd_disc_get_num_sessions;
  s->disc.get_session      = &chd_disc_get_session;
  s->disc.get_num_tracks   = &chd_disc_get_num_tracks;
  s->disc.get_track        = &chd_disc_get_track;
  s->disc.get_toc          = &chd_disc_get_toc;
  s->disc.read_sector      = &chd_disc_read_sector;

  return (struct disc *)s;
}
