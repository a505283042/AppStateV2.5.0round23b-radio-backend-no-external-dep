#include "web/web_snapshot.h"

#include "app_flags.h"
#include "app_state.h"
#include "audio/audio.h"
#include "audio/audio_service.h"
#include "lyrics/lyrics.h"
#include "player_playlist.h"
#include "player_state.h"
#include "player_source.h"
#include "audio/audio_radio_backend.h"
#include "radio/radio_catalog.h"
#include "storage/storage_catalog_v3.h"
#include "ui/ui.h"
#include "storage/storage_view_v3.h"
#include "web/web_config.h"
#include "web/web_settings.h"
#include "web/web_cover_cache.h"

static const char* web_mode_to_key(play_mode_t mode) {
  switch (mode) {
    case PLAY_MODE_ALL_SEQ:    return "all_seq";
    case PLAY_MODE_ALL_RND:    return "all_rnd";
    case PLAY_MODE_ARTIST_SEQ: return "artist_seq";
    case PLAY_MODE_ARTIST_RND: return "artist_rnd";
    case PLAY_MODE_ALBUM_SEQ:  return "album_seq";
    case PLAY_MODE_ALBUM_RND:  return "album_rnd";
    default:                   return "unknown";
  }
}

static const char* web_mode_to_label(play_mode_t mode) {
  switch (mode) {
    case PLAY_MODE_ALL_SEQ:    return "全部 · 顺序";
    case PLAY_MODE_ALL_RND:    return "全部 · 随机";
    case PLAY_MODE_ARTIST_SEQ: return "歌手 · 顺序";
    case PLAY_MODE_ARTIST_RND: return "歌手 · 随机";
    case PLAY_MODE_ALBUM_SEQ:  return "专辑 · 顺序";
    case PLAY_MODE_ALBUM_RND:  return "专辑 · 随机";
    default:                   return "未知";
  }
}

static const char* web_app_state_to_key(app_state_t s, bool rescanning) {
  if (rescanning) return "rescanning";
  switch (s) {
    case STATE_BOOT:      return "boot";
    case STATE_PLAYER:    return "player";
    case STATE_NFC_ADMIN: return "nfc_admin";
    case STATE_RADIO:     return "radio";
    default:              return "unknown";
  }
}

static const char* web_app_state_to_label(app_state_t s, bool rescanning) {
  if (rescanning) return "扫描中";
  switch (s) {
    case STATE_BOOT:      return "启动中";
    case STATE_PLAYER:    return "播放器";
    case STATE_NFC_ADMIN: return "NFC 管理";
    case STATE_RADIO:     return "收音机";
    default:              return "未知";
  }
}


static const char* web_view_to_key(ui_player_view_t v) {
  switch (v) {
    case UI_VIEW_ROTATE: return "rotate";
    case UI_VIEW_INFO:   return "info";
    default:             return "unknown";
  }
}

static const char* web_view_to_label(ui_player_view_t v) {
  switch (v) {
    case UI_VIEW_ROTATE: return "旋转视图";
    case UI_VIEW_INFO:   return "信息视图";
    default:             return "未知视图";
  }
}

static bool web_is_remote_image_url(const String& s) {
  return s.startsWith("http://") || s.startsWith("https://");
}

static uint32_t web_fnv1a32_add_bytes(uint32_t h, const char* s) {
  if (!s) return h;
  const uint8_t* p = reinterpret_cast<const uint8_t*>(s);
  while (*p) {
    h ^= *p++;
    h *= 16777619u;
  }
  return h;
}

static uint32_t web_fnv1a32_add_u32(uint32_t h, uint32_t v) {
  for (int i = 0; i < 4; ++i) {
    h ^= (uint8_t)((v >> (i * 8)) & 0xFF);
    h *= 16777619u;
  }
  return h;
}

static String web_make_track_cover_rev(const TrackViewV3& v) {
  uint32_t h = 2166136261u;
  h = web_fnv1a32_add_u32(h, (uint32_t)v.cover_source);
  h = web_fnv1a32_add_u32(h, v.cover_offset);
  h = web_fnv1a32_add_u32(h, v.cover_size);
  h = web_fnv1a32_add_bytes(h, v.audio_path.c_str());
  h = web_fnv1a32_add_bytes(h, v.cover_path.c_str());

  char buf[16];
  snprintf(buf, sizeof(buf), "%08lx", (unsigned long)h);
  return String(buf);
}
String web_make_radio_cover_rev(int radio_idx, const String& logo) {
  uint32_t h = 2166136261u;
  h = web_fnv1a32_add_u32(h, (uint32_t)(radio_idx >= 0 ? radio_idx : 0xFFFFFFFFu));
  h = web_fnv1a32_add_bytes(h, logo.c_str());

  char buf[24];
  snprintf(buf, sizeof(buf), "radio-%08lx", (unsigned long)h);
  return String(buf);
}
WebPlayerSnapshot web_snapshot_capture() {
  WebPlayerSnapshot snap{};
  snap.ok = true;
  snap.app_state = web_app_state_to_key(g_app_state, g_rescanning);
  snap.app_state_label = web_app_state_to_label(g_app_state, g_rescanning);
  snap.rescanning = g_rescanning;
  snap.is_playing = audio_service_is_playing();
  snap.is_paused = audio_service_is_paused();
  snap.play_ms = audio_get_play_ms();
  snap.total_ms = audio_get_total_ms();
  snap.volume = audio_get_volume();
  snap.mode = web_mode_to_key(g_play_mode);
  snap.current_group_idx = player_playlist_get_current_group_idx();
  snap.mode_label = web_mode_to_label(g_play_mode);
  snap.view = web_view_to_key(ui_get_view());
  snap.view_label = web_view_to_label(ui_get_view());
  const WebRuntimeSettings& ws = web_settings_get();
  snap.wifi_name = "-";
  snap.hostname = WEBCTRL_HOSTNAME_DEFAULT;
  snap.show_next_lyric = ws.show_next_lyric;
  snap.show_cover = ws.show_cover;
  snap.web_cover_spin = ws.web_cover_spin;
  snap.can_cancel_scan = g_rescanning && !g_abort_scan;
  snap.scan_action_label = g_rescanning ? (g_abort_scan ? "取消中..." : "取消重扫") : "开始重扫";

  snap.has_lyrics = g_lyricsDisplay.hasLyrics();
  if (snap.has_lyrics) {
      snap.current_lyric = g_lyricsDisplay.getCurrentLyricCStr();
      snap.next_lyric = g_lyricsDisplay.getNextLyricCStr();
      snap.following_lyric = g_lyricsDisplay.getFollowingLyricCStr();
      snap.current_lyric_start_ms = g_lyricsDisplay.getCurrentLyricStartTime();
      snap.next_lyric_start_ms = g_lyricsDisplay.getNextLyricStartTime();
      snap.following_lyric_start_ms = g_lyricsDisplay.getFollowingLyricStartTime();
  }

  const uint32_t base_poll = web_refresh_preset_poll_ms(ws.refresh_preset);
  if (snap.rescanning) {
    snap.next_poll_ms = min<uint32_t>(base_poll, WEBCTRL_STATUS_POLL_SCAN_MS);
  } else {
    snap.next_poll_ms = base_poll;
  }

  const int cur = player_state_current_index();
  snap.track_idx = cur;

  if (cur >= 0) {
    TrackViewV3 v{};
      if (storage_catalog_v3_get_track_view((uint32_t)cur, v, "/Music") && v.valid) {
        snap.title = v.title;
        snap.artist = v.artist;
        snap.album = v.album;

        snap.has_cover = (v.cover_source != COVER_NONE && (v.cover_size > 0 || v.cover_path.length() > 0));
        if (snap.has_cover) {
          snap.cover_rev = web_make_track_cover_rev(v);
          snap.cover_url = String("/api/cover/current?track=") + String(cur) +
                           "&rev=" + snap.cover_rev;
          snap.cover_ready_for_web = web_cover_cache_has(cur,
                                                         (CoverSource)v.cover_source,
                                                         v.audio_path.c_str(),
                                                         v.cover_path.c_str(),
                                                         v.cover_offset,
                                                         v.cover_size);
        } else {
          snap.cover_rev = "";
          snap.cover_url = "";
          snap.cover_ready_for_web = false;
        }

        const bool lyrics_expected = v.lrc_path.length() > 0;
        if (!snap.has_lyrics &&
            lyrics_expected &&
            !snap.rescanning &&
            snap.play_ms < 3000) {
          snap.lyrics_loading = true;
        }
      }

    const PlayerPlaylistDisplayInfo display =
        player_playlist_get_display_info(cur, (int)storage_catalog_v3_track_count());
    snap.display_pos = display.display_pos;
    snap.display_total = display.display_total;
  }

  const PlayerSourceState source = player_source_get();
  snap.source_type = player_source_type_key(source.type);
  snap.radio_active = source.radio_active;
  snap.radio_idx = source.radio_idx;
  snap.radio_name = source.radio_name;
  snap.radio_format = source.radio_format;
  snap.radio_region = source.radio_region;
  snap.radio_state = source.radio_state;
  snap.radio_error = source.radio_error;
  snap.radio_stream_title = source.radio_stream_title;
  snap.radio_backend = source.radio_backend;
  snap.radio_bitrate = source.radio_bitrate;

  if (source.type == PlayerSourceType::NET_RADIO) {
    const RadioBackendStatus rb = audio_radio_backend_get_status();
    if (rb.station.length()) snap.radio_name = rb.station;
    if (rb.stream_title.length()) snap.radio_stream_title = rb.stream_title;
    if (rb.bitrate > 0) snap.radio_bitrate = rb.bitrate;
    if (rb.error.length()) snap.radio_error = rb.error;
    if (rb.info.length() && snap.album.isEmpty()) snap.album = rb.info;
    snap.radio_backend = audio_radio_backend_name();
    snap.track_idx = -1;
    snap.title = snap.radio_name.length() ? snap.radio_name : String("网络电台");
    snap.artist = String("网络电台");
    snap.album = source.radio_region;
    snap.play_ms = 0;
    snap.total_ms = 0;
    snap.has_lyrics = false;
    snap.lyrics_loading = false;
    snap.current_lyric = "";
    snap.next_lyric = "";
    snap.following_lyric = "";
    snap.current_lyric_start_ms = 0;
    snap.next_lyric_start_ms = 0;
    snap.following_lyric_start_ms = 0;
    snap.cover_loading = false;
    String radio_logo = source.radio_logo;
    if (radio_logo.isEmpty() && source.radio_idx >= 0) {
        const RadioItem* item = radio_catalog_get((size_t)source.radio_idx);
        if (item && item->valid) {
            radio_logo = item->logo;
        }
    }

    snap.has_cover = radio_logo.length() > 0;
    if (snap.has_cover) {
      snap.cover_rev = web_make_radio_cover_rev(source.radio_idx, radio_logo);
      snap.cover_url = web_is_remote_image_url(radio_logo)
          ? radio_logo
          : (String("/api/radio/logo/current?idx=") + String(source.radio_idx) +
            "&rev=" + snap.cover_rev);
      snap.cover_ready_for_web = snap.has_cover;
    } else {
      snap.cover_url = String();
      snap.cover_rev = "";
      snap.cover_ready_for_web = false;
    }
    snap.display_pos = -1;
    snap.display_total = (int)radio_catalog_count();
  }

  return snap;
}
