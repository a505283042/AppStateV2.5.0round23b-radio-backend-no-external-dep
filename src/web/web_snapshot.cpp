#include "web/web_snapshot.h"

#include "app_flags.h"
#include "app_state.h"
#include "audio/audio.h"
#include "audio/audio_output_route.h"
#include "audio/audio_service.h"
#include "lyrics/lyrics.h"
#include "player_playlist.h"
#include "player_state.h"
#include "player_source.h"
#include "audio/audio_radio_backend.h"
#include "radio/radio_catalog.h"
#include "net_music/net_music_catalog.h"
#include "net_music/net_music_embedded_cover.h"
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
    default:              return "unknown";
  }
}

static const char* web_app_state_to_label(app_state_t s, bool rescanning) {
  if (rescanning) return "扫描中";
  switch (s) {
    case STATE_BOOT:      return "启动中";
    case STATE_PLAYER:    return "播放器";
    case STATE_NFC_ADMIN: return "NFC 管理";    
    default:              return "未知";
  }
}


static const char* web_view_to_key(ui_player_view_t v) {
  switch (v) {
    case UI_VIEW_ROTATE: return "rotate";
    case UI_VIEW_INFO:   return "info";
    case UI_VIEW_COVER_PANEL: return "cover_panel";
    default:             return "unknown";
  }
}

static const char* web_view_to_label(ui_player_view_t v) {
  switch (v) {
    case UI_VIEW_ROTATE: return "旋转视图";
    case UI_VIEW_INFO:   return "信息视图";
    case UI_VIEW_COVER_PANEL: return "封面面板";
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
static void web_snapshot_reset_for_capture(WebPlayerSnapshot& snap) {
  snap.ok = false;
  snap.app_state = "unknown";
  snap.app_state_label = "未知";
  snap.rescanning = false;
  snap.is_playing = false;
  snap.is_paused = false;
  snap.track_idx = -1;
  snap.title.remove(0);
  snap.artist.remove(0);
  snap.album.remove(0);
  snap.play_ms = 0;
  snap.total_ms = 0;
  snap.seekable = false;
  snap.seeking = false;
  snap.seek_target_ms = 0;
  snap.seek_actual_ms = 0;
  snap.seek_result = "idle";
  snap.seek_error.remove(0);
  snap.volume = 0;
  snap.mode = "unknown";
  snap.mode_label = "未知";
  snap.view = "unknown";
  snap.view_label = "未知视图";
  snap.display_pos = -1;
  snap.display_total = 0;
  snap.current_group_idx = -1;
  snap.can_cancel_scan = false;
  snap.scan_action_label = "开始重扫";

  snap.has_lyrics = false;
  snap.lyrics_loading = false;
  snap.current_lyric.remove(0);
  snap.next_lyric.remove(0);
  snap.following_lyric.remove(0);
  snap.show_next_lyric = true;
  snap.show_cover = true;
  snap.web_cover_spin = true;
  snap.current_lyric_start_ms = 0;
  snap.next_lyric_start_ms = 0;
  snap.following_lyric_start_ms = 0;
  snap.has_cover = false;
  snap.cover_loading = false;
  snap.cover_ready_for_web = false;
  snap.cover_rev.remove(0);
  snap.cover_url.remove(0);

  snap.source_type = "none";
  snap.radio_active = false;
  snap.radio_idx = -1;
  snap.radio_name.remove(0);
  snap.radio_format.remove(0);
  snap.radio_region.remove(0);
  snap.radio_state.remove(0);
  snap.radio_error.remove(0);
  snap.radio_stream_title.remove(0);
  snap.radio_backend.remove(0);
  snap.radio_bitrate = 0;

  snap.net_track_idx = -1;
  snap.net_track_title.remove(0);
  snap.net_track_state.remove(0);
  snap.net_track_error.remove(0);
  snap.next_poll_ms = 0;
}

void web_snapshot_capture_into(WebPlayerSnapshot& snap) {
  // 只清空长度，不释放 String 容量；网页高频轮询会复用历史缓冲区。
  web_snapshot_reset_for_capture(snap);
  snap.ok = true;
  const AppRescanState rescan = app_rescan_state_get();
  snap.app_state = web_app_state_to_key(g_app_state, rescan.rescanning);
  snap.app_state_label = web_app_state_to_label(g_app_state, rescan.rescanning);
  snap.rescanning = rescan.rescanning;
  snap.is_playing = audio_service_is_playing();
  snap.is_paused = audio_service_is_paused();
  snap.play_ms = audio_get_play_ms();
  snap.total_ms = audio_get_total_ms();
  AudioSeekStateSnapshot seek_state{};
  if (audio_service_get_seek_state(&seek_state)) {
    snap.seekable = seek_state.seekable;
    snap.seeking = seek_state.seeking;
    snap.seek_target_ms = seek_state.target_ms;
    snap.seek_actual_ms = seek_state.actual_ms;
    snap.seek_result = audio_seek_result_label(seek_state.result);
    snap.seek_error = seek_state.error;
  }
  snap.volume = audio_output_route_get_user_volume();
  const play_mode_t play_mode = app_play_mode_get();
  snap.mode = web_mode_to_key(play_mode);
  snap.current_group_idx = player_playlist_get_current_group_idx();
  snap.mode_label = web_mode_to_label(play_mode);
  snap.view = web_view_to_key(ui_get_view());
  snap.view_label = web_view_to_label(ui_get_view());
  const WebRuntimeSettings ws = web_settings_get();
  snap.show_next_lyric = ws.show_next_lyric;
  snap.show_cover = ws.show_cover;
  snap.web_cover_spin = ws.web_cover_spin;
  snap.can_cancel_scan = rescan.rescanning && !rescan.abort_requested;
  snap.scan_action_label = rescan.rescanning
      ? (rescan.abort_requested ? "取消中..." : "取消重扫")
      : "开始重扫";

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

  snap.net_track_idx = source.net_track_idx;
  snap.net_track_title = source.net_track_title;
  snap.net_track_state = source.net_track_state;
  snap.net_track_error = source.net_track_error;

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
    snap.seekable = false;
    snap.seeking = false;
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

  if (source.type == PlayerSourceType::NET_TRACK) {
    snap.track_idx = -1;

    snap.title = source.net_track_title.length()
                   ? source.net_track_title
                   : String("NAS歌曲");

    snap.artist = source.net_track_artist.length()
                    ? source.net_track_artist
                    : String("NAS");

    snap.album = source.net_track_album.length()
                   ? source.net_track_album
                   : String("网络音乐");

    snap.total_ms = source.net_track_duration_ms;

    CoverSource net_cover_source = COVER_NONE;
    uint32_t net_cover_offset = 0;
    uint32_t net_cover_size = 0;
    String net_cover_rev;
    const bool net_cover_known = net_music_embedded_cover_get_current(source.net_track_idx,
                                                                      source.net_track_url,
                                                                      &net_cover_source,
                                                                      &net_cover_offset,
                                                                      &net_cover_size,
                                                                      &net_cover_rev);
    const bool net_cover_ready = net_cover_known &&
        web_cover_cache_has(source.net_track_idx,
                            net_cover_source,
                            source.net_track_url.c_str(),
                            "",
                            net_cover_offset,
                            net_cover_size);

    snap.cover_loading = !net_cover_ready && source.net_track_active;
    snap.has_cover = net_cover_ready;
    snap.cover_ready_for_web = net_cover_ready;
    snap.cover_rev = net_cover_ready ? net_cover_rev : String();
    snap.cover_url = net_cover_ready
        ? (String("/api/cover/current?net=1&idx=") + String(source.net_track_idx) +
           "&rev=" + net_cover_rev)
        : String();

    snap.display_pos = source.net_track_idx;
    snap.display_total = (int)net_music_catalog_count();
  }

}

WebPlayerSnapshot web_snapshot_capture() {
  WebPlayerSnapshot snap{};
  web_snapshot_capture_into(snap);
  return snap;
}
