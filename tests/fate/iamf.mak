FATE_IAMF += fate-iamf-stereo
fate-iamf-stereo: tests/data/asynth-44100-2.wav tests/data/streamgroups/audio_element-stereo tests/data/streamgroups/mix_presentation-stereo
fate-iamf-stereo: SRC = $(TARGET_PATH)/tests/data/asynth-44100-2.wav
fate-iamf-stereo: CMD = transcode wav $(SRC) iamf " \
  -/stream_group $(TARGET_PATH)/tests/data/streamgroups/audio_element-stereo \
  -/stream_group $(TARGET_PATH)/tests/data/streamgroups/mix_presentation-stereo \
  -streamid 0:0 -c:a flac -t 1" "-c:a copy -map 0"

FATE_IAMF += fate-iamf-5_1_4
fate-iamf-5_1_4: tests/data/asynth-44100-10.wav tests/data/filtergraphs/iamf_5_1_4 tests/data/streamgroups/audio_element-5_1_4 tests/data/streamgroups/mix_presentation-5_1_4
fate-iamf-5_1_4: SRC = $(TARGET_PATH)/tests/data/asynth-44100-10.wav
fate-iamf-5_1_4: CMD = transcode wav $(SRC) iamf "-auto_conversion_filters \
  -/filter_complex $(TARGET_PATH)/tests/data/filtergraphs/iamf_5_1_4 \
  -/stream_group $(TARGET_PATH)/tests/data/streamgroups/audio_element-5_1_4 \
  -/stream_group $(TARGET_PATH)/tests/data/streamgroups/mix_presentation-5_1_4 \
  -streamid 0:0 -streamid 1:1 -streamid 2:2 -streamid 3:3 -streamid 4:4 -streamid 5:5 -map [FRONT] -map [BACK] -map [CENTER] -map [LFE] -map [TOP_FRONT] -map [TOP_BACK] -c:a flac -t 1" "-c:a copy -map 0"

FATE_IAMF += fate-iamf-7_1_4
fate-iamf-7_1_4: tests/data/asynth-44100-12.wav tests/data/filtergraphs/iamf_7_1_4 tests/data/streamgroups/audio_element-7_1_4 tests/data/streamgroups/mix_presentation-7_1_4
fate-iamf-7_1_4: SRC = $(TARGET_PATH)/tests/data/asynth-44100-12.wav
fate-iamf-7_1_4: CMD = transcode wav $(SRC) iamf "-auto_conversion_filters \
  -/filter_complex $(TARGET_PATH)/tests/data/filtergraphs/iamf_7_1_4 \
  -/stream_group $(TARGET_PATH)/tests/data/streamgroups/audio_element-7_1_4 \
  -/stream_group $(TARGET_PATH)/tests/data/streamgroups/mix_presentation-7_1_4 \
  -streamid 0:0 -streamid 1:1 -streamid 2:2 -streamid 3:3 -streamid 4:4 -streamid 5:5 -streamid 6:6 -map [FRONT] -map [BACK] -map [CENTER] -map [LFE] -map [SIDE] -map [TOP_FRONT] -map [TOP_BACK] -c:a flac -t 1" "-c:a copy -map 0"

FATE_IAMF-$(call TRANSCODE, FLAC, IAMF, WAV_DEMUXER PCM_S16LE_DECODER) += $(FATE_IAMF)

FATE_FFMPEG += $(FATE_IAMF-yes)

fate-iamf: $(FATE_IAMF-yes)
