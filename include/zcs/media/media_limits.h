/**
 * @file media_limits.h
 * @brief 媒体尺寸/缓冲常量与限制。
 */
#ifndef ZCS_MEDIA_MEDIA_LIMITS_H
#define ZCS_MEDIA_MEDIA_LIMITS_H

/**
 * ZCS 媒体层策略常量（非协议规范魔数）。
 * 协议字面量见 rtmp.h / rtp.h / h264_over_rtp.c 等。
 */

/** 每个 SDP / payload bank / demux pipeline 的最大并发媒体轨数。 */
#define ZCS_TRACK_SLOT_MAX 4u

/** 视频宽度下限：低于此值视为无效/占位轨道（信令与统计共用）。 */
#define ZCS_VIDEO_WIDTH_MIN_VALID 64u

/** RTMP/RTSP/channel 大 I/O 缓冲（64KB）。 */
#define ZCS_MEDIA_IO_BUF_SIZE 65536u

/** WebRTC WHEP 播放出站（与 RTSP play_sender 调参对齐；仅 WebRTC）。 */
#define ZCS_WEBRTC_PLAY_RTPQ_CAP 512u
#define ZCS_WEBRTC_PLAY_RTPQ_HIGH_WATER 128u
/** 每会话 RTP 发送队列字节上限；慢消费者只影响自身。 */
#define ZCS_WEBRTC_PLAY_RTPQ_BYTES (512u * 1024u)
/** NACK 重传缓存：只缓存视频包，按包数/字节双上限。 */
#define ZCS_WEBRTC_NACK_CACHE_PKTS 256u
#define ZCS_WEBRTC_NACK_CACHE_BYTES (256u * 1024u)
#define ZCS_WEBRTC_PLAY_RTP_SLOT_MAX 2048u
#define ZCS_WEBRTC_PLAY_RTP_FLUSH 256
#define ZCS_WEBRTC_PLAY_CRYPT_BYTES 4096u
#define ZCS_WEBRTC_PLAY_DTLS_IO 2048u
#define ZCS_WEBRTC_PLAY_H264_STAP_MAX 512u

/** 并发 WHEP/WHIP 会话数与 zice agent→poller 映射容量的**硬上限**。
 *  实际允许值由 [webrtc] max_sessions 配置决定（缺省 64、上限即此值）；
 *  会话表是用最大容量一次分配，改这里会同时抬高内存占用（每档约 100B），
 *  所以日常调并发请改配置而不是改这个宏。 */
#define ZCS_WEBRTC_SESSION_MAX_HARD 1024u
/** 未配 [webrtc] max_sessions 时的缺省并发会话数。 */
#define ZCS_WEBRTC_SESSION_MAX_DEFAULT 64u
/** ICE agent→poller 反查表容量：与硬上限一致即可覆盖所有会话。 */
#define ZCS_WEBRTC_ICE_AGENT_MAX ZCS_WEBRTC_SESSION_MAX_HARD
/** 会话空闲回收：未完成握手 / 长时间无入站流量。 */
#define ZCS_WEBRTC_SESSION_CONNECT_TIMEOUT_MS 30000u
#define ZCS_WEBRTC_SESSION_IDLE_TIMEOUT_MS 120000u
#define ZCS_WEBRTC_REAP_INTERVAL_MS 5000u

/** VOD 容器 demux 初始缓冲；4K H.264 高码率 IDR 可达约 1.5MB，
 *  8K H.265 IDR 可达约 2MB。取 2MB 避免 mpegps/mpegts 静默丢帧。 */
#define ZCS_VOD_DEMUX_READ_MIN (2u * 1024u * 1024u)

/** MPEG-TS（SRT 推流）H264 访问单元上限；1080p IDR 常见约 10000KB。 */
#define ZCS_MPEGTS_AU_MAX ZCS_VOD_DEMUX_READ_MIN

#define ZCS_MEDIA_TS_REWIND_THRESHOLD_MS 5000u
/** 时间戳单调步进（gop_queue）。 */
#define ZCS_MEDIA_TS_MIN_STEP_MS 40u
/** AAC-LC 1024 samples @ 44.1kHz */
#define ZCS_AAC_FRAME_MS 23u
/** 音轨相对视频允许的最大落后（毫秒） */
#define ZCS_AV_SYNC_LAG_MS 200u

/** read_muxed A/V 交织偏差上限（与 sync lag 预算相同）。 */
#define ZCS_GOP_QUEUE_MUX_MAX_SKEW_MS ZCS_AV_SYNC_LAG_MS

/** RTP 排序/重排保持时间（毫秒），缺口跳过前；UDP 需更大，TCP interleaved 更小。 */
#define ZCS_RTP_JITTER_MS_UDP_DEFAULT 200
#define ZCS_RTP_JITTER_MS_TCP_DEFAULT 50

/*
 * GOP 队列容量/读 lag 策略常量（CAPACITY / MAX_GOP / TARGET_GOPS /
 * MAX_READERS / PLAY_MAX_LAG）统一定义于 zcs/engine/gop/gop_limits.h，
 * 由 gop_queue.h 引入，避免多处重复定义。
 */

#endif /* ZCS_MEDIA_MEDIA_LIMITS_H */
