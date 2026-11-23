#!/bin/bash
# VIDEO FOR THE TAB5 - converts any video into what the Video app plays:
#   <base>.avi  MJPEG video, 720 wide (the Tab5's screen), at most 25
#               frames per second, NO audio
#   <base>.mp3  the audio, 44100 Hz, stereo, 128 kbps
# Copy BOTH to the Videos folder on the SD card: the app plays the .avi and the audio from the
# .mp3 of the same name (the audio rules the video's timing).
#
# Usage: ferramentas/video_para_tab5.sh <entrada> [saida_base]
#   ferramentas/video_para_tab5.sh filme.mkv              -> filme.avi + filme.mp3 (in the current folder)
#   ferramentas/video_para_tab5.sh filme.mkv /Volumes/SD/Videos/filme
#
# Needs ffmpeg:  brew install ffmpeg
set -e

if ! command -v ffmpeg > /dev/null 2>&1 || ! command -v ffprobe > /dev/null 2>&1; then
  echo "ffmpeg not found. Install it with:  brew install ffmpeg" >&2
  exit 1
fi
if [ -z "$1" ] || [ ! -f "$1" ]; then
  echo "usage: $0 <entrada> [saida_base]" >&2
  exit 1
fi

ENTRADA="$1"
# Without the second argument: the input name, without the extension, in the current folder.
BASE="${2:-$(basename "${ENTRADA%.*}")}"

# FRAMES PER SECOND: the input's, but never more than 25 (the Tab5's decoder
# does not get much beyond that at 720 wide).
FPS_ORIGINAL=$(ffprobe -v error -select_streams v:0 -show_entries stream=r_frame_rate \
               -of default=noprint_wrappers=1:nokey=1 "$ENTRADA" | head -1)
FPS=$(awk -v f="$FPS_ORIGINAL" 'BEGIN {
  n = split(f, a, "/"); v = (n == 2 && a[2] > 0) ? a[1] / a[2] : a[1] + 0
  if (v <= 0 || v > 25) v = 25
  printf "%.3f", v }')

# THE VIDEO: width 720 and a proportional even height (scale=720:-2); MJPEG with
# 4:2:0 chroma (what the P4 decoder reads); -q:v 5 = good quality, frames
# of ~40 KB; -an = no audio (the audio goes in the .mp3).
echo "video: ${BASE}.avi  (720 wide, ${FPS} frames/s)"
ffmpeg -v error -stats -y -i "$ENTRADA" -an \
  -vf "scale=720:-2,fps=${FPS}" -c:v mjpeg -pix_fmt yuvj420p -q:v 5 "${BASE}.avi"

# THE AUDIO: only if the input has an audio track.
if [ -n "$(ffprobe -v error -select_streams a:0 -show_entries stream=codec_type \
           -of default=noprint_wrappers=1:nokey=1 "$ENTRADA")" ]; then
  echo "audio: ${BASE}.mp3  (44100 Hz, stereo, 128 kbps)"
  ffmpeg -v error -stats -y -i "$ENTRADA" -vn -ar 44100 -ac 2 -c:a libmp3lame -b:a 128k "${BASE}.mp3"
else
  echo "the input has no audio: only the .avi was generated (plays silent)"
fi

echo "done. Copy to the Videos folder on the SD card: ${BASE}.avi and ${BASE}.mp3"
