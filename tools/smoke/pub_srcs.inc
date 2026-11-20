# Sourced by the host tests that build the replay harness (tools/smoke/test_log_replay.c with the walker's rsx_commands.c): patch 2197
# made the funnel call the publisher (libs/video/rsx_pub.c, over libs/kzr), so the harness links them too.
#   pub_src_args VIDEO_DIR KZR_DIR   -> PUB_SRC=(sources and -I flags); empty when the tree has no rsx_pub.c (a pre-2197 baseline)
pub_src_args() {
  PUB_SRC=()
  [[ -f "$1/rsx_pub.c" && -f "$2/core/sync/kzr_sync.c" ]] || return 0
  PUB_SRC=("$1/rsx_pub.c" "$2/core/sync/kzr_sync.c" "$2/platform/common/kzr_event.c" "$2/platform/posix/kzr_platform_posix.c" -I "$2/include")
}
