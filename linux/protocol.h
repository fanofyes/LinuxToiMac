/*
 * Wire protocol between vscreen-send (Linux) and VScreen.app (macOS).
 *
 * TCP, one connection. The sender connects to the receiver.
 * Every message starts with a 20-byte big-endian header:
 *
 *   u32 magic        'VSCR'
 *   u8  type         VS_MSG_*
 *   u8  flags        VS_FLAG_*
 *   u16 reserved     0
 *   u32 payload_len
 *   u64 pts_us       sender capture time (CLOCK_MONOTONIC, microseconds)
 *
 * sender -> receiver:
 *   VS_MSG_HELLO   payload: u16 version, u16 codec (1 = H.264), u32 width, u32 height
 *   VS_MSG_VIDEO   payload: one H.264 access unit in Annex-B format.
 *                  SPS/PPS are repeated in-band before every IDR frame.
 * receiver -> sender:
 *   VS_MSG_KEYFRAME_REQ   no payload
 */
#ifndef VSCREEN_PROTOCOL_H
#define VSCREEN_PROTOCOL_H

#define VS_MAGIC 0x56534352u
#define VS_VERSION 1
#define VS_DEFAULT_PORT 7310
#define VS_HDR_LEN 20

#define VS_MSG_HELLO 1
#define VS_MSG_VIDEO 2
#define VS_MSG_KEYFRAME_REQ 3

#define VS_FLAG_KEYFRAME 0x01

#define VS_CODEC_H264 1

#endif
