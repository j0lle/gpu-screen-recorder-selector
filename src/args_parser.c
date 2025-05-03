#include "../include/args_parser.h"
#include "../include/defs.h"
#include "../include/egl.h"
#include "../include/window/window.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <limits.h>
#include <assert.h>
#include <libgen.h>
#include <sys/stat.h>

#ifndef GSR_VERSION
#define GSR_VERSION "unknown"
#endif

static const ArgEnum video_codec_enums[] = {
    { .name = "auto",        .value = GSR_VIDEO_CODEC_AUTO       },
    { .name = "h264",        .value = GSR_VIDEO_CODEC_H264       },
    { .name = "h265",        .value = GSR_VIDEO_CODEC_HEVC       },
    { .name = "hevc",        .value = GSR_VIDEO_CODEC_HEVC       },
    { .name = "hevc_hdr",    .value = GSR_VIDEO_CODEC_HEVC_HDR   },
    { .name = "hevc_10bit",  .value = GSR_VIDEO_CODEC_HEVC_10BIT },
    { .name = "av1",         .value = GSR_VIDEO_CODEC_AV1        },
    { .name = "av1_hdr",     .value = GSR_VIDEO_CODEC_AV1_HDR    },
    { .name = "av1_10bit",   .value = GSR_VIDEO_CODEC_AV1_10BIT  },
    { .name = "vp8",         .value = GSR_VIDEO_CODEC_VP8        },
    { .name = "vp9",         .value = GSR_VIDEO_CODEC_VP9        },
};

static const ArgEnum audio_codec_enums[] = {
    { .name = "opus", .value = GSR_AUDIO_CODEC_OPUS },
    { .name = "aac",  .value = GSR_AUDIO_CODEC_AAC  },
    { .name = "flac", .value = GSR_AUDIO_CODEC_FLAC },
};

static const ArgEnum video_encoder_enums[] = {
    { .name = "gpu", .value = GSR_VIDEO_ENCODER_HW_GPU },
    { .name = "cpu", .value = GSR_VIDEO_ENCODER_HW_CPU },
};

static const ArgEnum pixel_format_enums[] = {
    { .name = "yuv420", .value = GSR_PIXEL_FORMAT_YUV420 },
    { .name = "yuv444", .value = GSR_PIXEL_FORMAT_YUV444 },
};

static const ArgEnum framerate_mode_enums[] = {
    { .name = "vfr",     .value = GSR_FRAMERATE_MODE_VARIABLE },
    { .name = "cfr",     .value = GSR_FRAMERATE_MODE_CONSTANT },
    { .name = "content", .value = GSR_FRAMERATE_MODE_CONTENT  },
};

static const ArgEnum bitrate_mode_enums[] = {
    { .name = "auto", .value = GSR_BITRATE_MODE_AUTO },
    { .name = "qp",   .value = GSR_BITRATE_MODE_QP   },
    { .name = "cbr",  .value = GSR_BITRATE_MODE_CBR  },
    { .name = "vbr",  .value = GSR_BITRATE_MODE_VBR  },
};

static const ArgEnum color_range_enums[] = {
    { .name = "limited", .value = GSR_COLOR_RANGE_LIMITED },
    { .name = "full",    .value = GSR_COLOR_RANGE_FULL    },
};

static const ArgEnum tune_enums[] = {
    { .name = "performance", .value = GSR_TUNE_PERFORMANCE },
    { .name = "quality",     .value = GSR_TUNE_QUALITY     },
};

static void arg_deinit(Arg *arg) {
    if(arg->values) {
        free(arg->values);
        arg->values = NULL;
    }
}

static bool arg_append_value(Arg *arg, const char *value) {
    if(arg->num_values + 1 >= arg->capacity_num_values) {
        const int new_capacity_num_values = arg->capacity_num_values == 0 ? 4 : arg->capacity_num_values*2;
        void *new_data = realloc(arg->values, new_capacity_num_values * sizeof(const char*));
        if(!new_data)
            return false;

        arg->values = new_data;
        arg->capacity_num_values = new_capacity_num_values;
    }

    arg->values[arg->num_values] = value;
    ++arg->num_values;
    return true;
}

static bool arg_get_enum_value_by_name(const Arg *arg, const char *name, int *enum_value) {
    assert(arg->type == ARG_TYPE_ENUM);
    assert(arg->enum_values);
    for(int i = 0; i < arg->num_enum_values; ++i) {
        if(strcmp(arg->enum_values[i].name, name) == 0) {
            *enum_value = arg->enum_values[i].value;
            return true;
        }
    }
    return false;
}

static void arg_print_expected_enum_names(const Arg *arg) {
    assert(arg->type == ARG_TYPE_ENUM);
    assert(arg->enum_values);
    for(int i = 0; i < arg->num_enum_values; ++i) {
        if(i > 0) {
            if(i == arg->num_enum_values -1)
                fprintf(stderr, " or ");
            else
                fprintf(stderr, ", ");
        }
        fprintf(stderr, "'%s'", arg->enum_values[i].name);
    }
}

static Arg* args_get_by_key(Arg *args, int num_args, const char *key) {
    for(int i = 0; i < num_args; ++i) {
        if(strcmp(args[i].key, key) == 0)
            return &args[i];
    }
    return NULL;
}

static const char* args_get_value_by_key(Arg *args, int num_args, const char *key) {
    for(int i = 0; i < num_args; ++i) {
        if(strcmp(args[i].key, key) == 0) {
            if(args[i].num_values == 0)
                return NULL;
            else
                return args[i].values[0];
        }
    }
    return NULL;
}

static bool args_get_boolean_by_key(Arg *args, int num_args, const char *key, bool default_value) {
    Arg *arg = args_get_by_key(args, num_args, key);
    assert(arg);
    if(arg->num_values == 0) {
        return default_value;
    } else {
        assert(arg->type == ARG_TYPE_BOOLEAN);
        return arg->typed_value.boolean;
    }
}

static int args_get_enum_by_key(Arg *args, int num_args, const char *key, int default_value) {
    Arg *arg = args_get_by_key(args, num_args, key);
    assert(arg);
    if(arg->num_values == 0) {
        return default_value;
    } else {
        assert(arg->type == ARG_TYPE_ENUM);
        return arg->typed_value.enum_value;
    }
}

static int64_t args_get_i64_by_key(Arg *args, int num_args, const char *key, int64_t default_value) {
    Arg *arg = args_get_by_key(args, num_args, key);
    assert(arg);
    if(arg->num_values == 0) {
        return default_value;
    } else {
        assert(arg->type == ARG_TYPE_I64);
        return arg->typed_value.i64_value;
    }
}

static double args_get_double_by_key(Arg *args, int num_args, const char *key, double default_value) {
    Arg *arg = args_get_by_key(args, num_args, key);
    assert(arg);
    if(arg->num_values == 0) {
        return default_value;
    } else {
        assert(arg->type == ARG_TYPE_DOUBLE);
        return arg->typed_value.d_value;
    }
}

static void usage_header() {
    const bool inside_flatpak = getenv("FLATPAK_ID") != NULL;
    const char *program_name = inside_flatpak ? "flatpak run --command=gpu-screen-recorder com.dec05eba.gpu_screen_recorder" : "gpu-screen-recorder";
    printf("usage: %s -w <window_id|monitor|focused|portal|region> [-c <container_format>] [-s WxH] [-region WxH+X+Y] [-f <fps>] [-a <audio_input>] [-q <quality>] [-r <replay_buffer_size_sec>] [-restart-replay-on-save yes|no] [-k h264|hevc|av1|vp8|vp9|hevc_hdr|av1_hdr|hevc_10bit|av1_10bit] [-ac aac|opus|flac] [-ab <bitrate>] [-oc yes|no] [-fm cfr|vfr|content] [-bm auto|qp|vbr|cbr] [-cr limited|full] [-tune performance|quality] [-df yes|no] [-sc <script_path>] [-cursor yes|no] [-keyint <value>] [-restore-portal-session yes|no] [-portal-session-token-filepath filepath] [-encoder gpu|cpu] [-o <output_file>] [-ro <output_directory>] [--list-capture-options [card_path]] [--list-audio-devices] [--list-application-audio] [-v yes|no] [-gl-debug yes|no] [--version] [-h|--help]\n", program_name);
    fflush(stdout);
}

// TODO: Update with portal info
static void usage_full() {
    const bool inside_flatpak = getenv("FLATPAK_ID") != NULL;
    const char *program_name = inside_flatpak ? "flatpak run --command=gpu-screen-recorder com.dec05eba.gpu_screen_recorder" : "gpu-screen-recorder";
    usage_header();
    printf("\n");
    printf("OPTIONS:\n");
    printf("  -w    Window id to record, a display (monitor name), \"screen\", \"screen-direct\", \"focused\", \"portal\" or \"region\".\n");
    printf("        If this is \"portal\" then xdg desktop screencast portal with PipeWire will be used. Portal option is only available on Wayland.\n");
    printf("        If you select to save the session (token) in the desktop portal capture popup then the session will be saved for the next time you use \"portal\",\n");
    printf("        but the session will be ignored unless you run GPU Screen Recorder with the '-restore-portal-session yes' option.\n");
    printf("        If this is \"region\" then the region specified by the -region option is recorded.\n");
    printf("        If this is \"screen\" then the first monitor found is recorded.\n");
    printf("        \"screen-direct\" can only be used on Nvidia X11, to allow recording without breaking VRR (G-SYNC). This also records all of your monitors.\n");
    printf("        Using this \"screen-direct\" option is not recommended unless you use VRR (G-SYNC) as there are Nvidia driver issues that can cause your system or games to freeze/crash.\n");
    printf("        The \"screen-direct\" option is not needed on AMD, Intel nor Nvidia on Wayland as VRR works properly in those cases.\n");
    printf("        Run GPU Screen Recorder with the --list-capture-options option to list valid values for this option.\n");
    printf("\n");
    printf("  -c    Container format for output file, for example mp4, or flv. Only required if no output file is specified or if recording in replay buffer mode.\n");
    printf("        If an output file is specified and -c is not used then the container format is determined from the output filename extension.\n");
    printf("        Only containers that support h264, hevc, av1, vp8 or vp9 are supported, which means that only mp4, mkv, flv, webm (and some others) are supported.\n");
    printf("\n");
    printf("  -s    The output resolution limit of the video in the format WxH, for example 1920x1080. If this is 0x0 then the original resolution is used. Optional, except when -w is \"focused\".\n");
    printf("        Note: the captured content is scaled to this size. The output resolution might not be exactly as specified by this option. The original aspect ratio is respected so the resolution will match that.\n");
    printf("        The video encoder might also need to add padding, which will result in black bars on the sides of the video. This is especially an issue on AMD.\n");
    printf("\n");
    printf("  -region\n");
    printf("        The region to capture, only to be used with -w region. This is in format WxH+X+Y, which is compatible with tools such as slop (X11) and slurp (kde plasma, wlroots and hyprland).\n");
    printf("        The region can be inside any monitor. If width and height are 0 (for example 0x0+500+500) then the entire monitor that the region is inside in will be recorded.\n");
    printf("        Note: currently the region can't span multiple monitors.\n");
    printf("\n");
    printf("  -f    Frame rate to record at. Recording will only capture frames at this target frame rate.\n");
    printf("        For constant frame rate mode this option is the frame rate every frame will be captured at and if the capture frame rate is below this target frame rate then the frames will be duplicated.\n");
    printf("        For variable frame rate mode this option is the max frame rate and if the capture frame rate is below this target frame rate then frames will not be duplicated.\n");
    printf("        Content frame rate is similar to variable frame rate mode, except the frame rate will match the frame rate of the captured content when possible, but not capturing above the frame rate set in this -f option.\n");
    printf("        Optional, set to 60 by default.\n");
    printf("\n");
    printf("  -a    Audio device or application to record from (pulse audio device). Can be specified multiple times. Each time this is specified a new audio track is added for the specified audio device or application.\n");
    printf("        The audio device can also be \"default_output\" in which case the default output device is used, or \"default_input\" in which case the default input device is used.\n");
    printf("        Multiple audio sources can be merged into one audio track by using \"|\" as a separator into one -a argument, for example: -a \"default_output|default_input\".\n");
    printf("        The audio name can also be prefixed with \"device:\", for example: -a \"device:default_output\".\n");
    printf("        To record audio from an application then prefix the audio name with \"app:\", for example: -a \"app:Brave\". The application name is case-insensitive.\n");
    printf("        To record audio from all applications except the provided ones prefix the audio name with \"app-inverse:\", for example: -a \"app-inverse:Brave\".\n");
    printf("        \"app:\" and \"app-inverse:\" can't be mixed in one audio track.\n");
    printf("        One audio track can contain both audio devices and application audio, for example: -a \"default_output|device:alsa_output.pci-0000_00_1b.0.analog-stereo.monitor|app:Brave\".\n");
    printf("        Recording application audio is only possible when the sound server on the system is PipeWire.\n");
    printf("        If the audio name is an empty string then the argument is ignored.\n");
    printf("        Optional, no audio track is added by default.\n");
    printf("        Run GPU Screen Recorder with the --list-audio-devices option to list valid audio device names.\n");
    printf("        Run GPU Screen Recorder with the --list-application-audio option to list valid application names. It's possible to use an application name that is not listed in --list-application-audio,\n");
    printf("        for example when trying to record audio from an application that hasn't started yet.\n");
    printf("\n");
    printf("  -q    Video quality. Should be either 'medium', 'high', 'very_high' or 'ultra' when using '-bm qp' or '-bm vbr' options, and '-bm qp' is the default option used.\n");
    printf("        'high' is the recommended option when live streaming or when you have a slower harddrive.\n");
    printf("        When using '-bm cbr' option then this is option is instead used to specify the video bitrate in kbps.\n");
    printf("        Optional when using '-bm qp' or '-bm vbr' options, set to 'very_high' be default.\n");
    printf("        Required when using '-bm cbr' option.\n");
    printf("\n");
    printf("  -r    Replay buffer time in seconds. If this is set, then only the last seconds as set by this option will be stored\n");
    printf("        and the video will only be saved when the gpu-screen-recorder is closed. This feature is similar to Nvidia's instant replay feature This option has be between 5 and 1200.\n");
    printf("        Note that the video data is stored in RAM, so don't use too long replay buffer time and use constant bitrate option (-bm cbr) to prevent RAM usage from going too high in busy scenes.\n");
    printf("        Optional, disabled by default.\n");
    printf("\n");
    printf("  -restart-replay-on-save\n");
    printf("        Restart replay on save. For example if this is set to 'no' and replay time (-r) is set to 60 seconds and a replay is saved once then the first replay video is 60 seconds long\n");
    printf("        and if a replay is saved 10 seconds later then the second replay video will also be 60 seconds long and contain 50 seconds of the previous video as well.\n");
    printf("        If this is set to 'yes' then after a replay is saved the replay buffer data is cleared and the second replay will start from that point onward.\n");
    printf("        The replay is only restarted when saving a full replay (SIGUSR1 signal)\n");
    printf("        Optional, set to 'no' by default.\n");
    printf("\n");
    printf("  -k    Video codec to use. Should be either 'auto', 'h264', 'hevc', 'av1', 'vp8', 'vp9', 'hevc_hdr', 'av1_hdr', 'hevc_10bit' or 'av1_10bit'.\n");
    printf("        Optional, set to 'auto' by default which defaults to 'h264'. Forcefully set to 'h264' if the file container type is 'flv'.\n");
    printf("        'hevc_hdr' and 'av1_hdr' option is not available on X11 nor when using the portal capture option.\n");
    printf("        'hevc_10bit' and 'av1_10bit' options allow you to select 10 bit color depth which can reduce banding and improve quality in darker areas, but not all video players support 10 bit color depth\n");
    printf("        and if you upload the video to a website the website might reduce 10 bit to 8 bit.\n");
    printf("        Note that when using 'hevc_hdr' or 'av1_hdr' the color depth is also 10 bits.\n");
    printf("\n");
    printf("  -ac   Audio codec to use. Should be either 'aac', 'opus' or 'flac'. Optional, set to 'opus' for .mp4/.mkv files, otherwise set to 'aac'.\n");
    printf("        'opus' and 'flac' is only supported by .mp4/.mkv files. 'opus' is recommended for best performance and smallest audio size.\n");
    printf("        Flac audio codec is option is disable at the moment because of a temporary issue.\n");
    printf("\n");
    printf("  -ab   Audio bitrate in kbps. If this is set to 0 then it's the same as if it's absent, in which case the bitrate is determined automatically depending on the audio codec.\n");
    printf("        Optional, by default the bitrate is 128kbps for opus and flac and 160kbps for aac.\n");
    printf("\n");
    printf("  -oc   Overclock memory transfer rate to the maximum performance level. This only applies to NVIDIA on X11 and exists to overcome a bug in NVIDIA driver where performance level\n");
    printf("        is dropped when you record a game. Only needed if you are recording a game that is bottlenecked by GPU. The same issue exists on Wayland but overclocking is not possible on Wayland.\n");
    printf("        Works only if your have \"Coolbits\" set to \"12\" in NVIDIA X settings, see README for more information. Note! use at your own risk! Optional, disabled by default.\n");
    printf("\n");
    printf("  -fm   Framerate mode. Should be either 'cfr' (constant frame rate), 'vfr' (variable frame rate) or 'content'. Optional, set to 'vfr' by default.\n");
    printf("        'vfr' is recommended for recording for less issue with very high system load but some applications such as video editors may not support it properly.\n");
    printf("        'content' is currently only supported on X11 or when using portal capture option. The 'content' option matches the recording frame rate to the captured content.\n");
    printf("\n");
    printf("  -bm   Bitrate mode. Should be either 'auto', 'qp' (constant quality), 'vbr' (variable bitrate) or 'cbr' (constant bitrate). Optional, set to 'auto' by default which defaults to 'qp' on all devices\n");
    printf("        except steam deck that has broken drivers and doesn't support qp.\n");
    printf("        Note: 'vbr' option is not supported when using '-encoder cpu' option.\n");
    printf("\n");
    printf("  -cr   Color range. Should be either 'limited' (aka mpeg) or 'full' (aka jpeg). Optional, set to 'limited' by default.\n");
    printf("        Limited color range means that colors are in range 16-235 (4112-60395 for hdr) while full color range means that colors are in range 0-255 (0-65535 for hdr).\n");
    printf("        Note that some buggy video players (such as vlc) are unable to correctly display videos in full color range and when upload the video to websites the website\n");
    printf("        might re-encoder the video to make the video limited color range.\n");
    printf("\n");
    printf("  -tune\n");
    printf("        Tune for performance or quality. Should be either 'performance' or 'quality'. At the moment this option only has an effect on Nvidia where setting this to quality\n");
    printf("        sets options such as preset, multipass and b frames. Optional, set to 'performance' by default.\n");
    printf("\n");
    printf("  -df   Organise replays in folders based on the current date.\n");
    printf("\n");
    printf("  -sc   Run a script on the saved video file (asynchronously). The first argument to the script is the filepath to the saved video file and the second argument is the recording type (either \"regular\" or \"replay\").\n");
    printf("        Not applicable for live streams.\n");
    printf("\n");
    printf("  -cursor\n");
    printf("        Record cursor. Optional, set to 'yes' by default.\n");
    printf("\n");
    printf("  -keyint\n");
    printf("        Specifies the keyframe interval in seconds, the max amount of time to wait to generate a keyframe. Keyframes can be generated more often than this.\n");
    printf("        This also affects seeking in the video and may affect how the replay video is cut. If this is set to 10 for example then you can only seek in 10-second chunks in the video.\n");
    printf("        Setting this to a higher value reduces the video file size if you are ok with the previously described downside. This option is expected to be a floating point number.\n");
    printf("        By default this value is set to 2.0.\n");
    printf("\n");
    printf("  -restore-portal-session\n");
    printf("        If GPU Screen Recorder should use the same capture option as the last time. Using this option removes the popup asking what you want to record the next time you record with '-w portal'\n");
    printf("        if you selected the option to save session (token) in the desktop portal screencast popup.\n");
    printf("        This option may not have any effect on your Wayland compositor and your systems desktop portal needs to support ScreenCast version 5 or later. Optional, set to 'no' by default.\n");
    printf("\n");
    printf("  -portal-session-token-filepath\n");
    printf("        This option is used together with -restore-portal-session option to specify the file path to save/restore the portal session token to/from.\n");
    printf("        This can be used to remember different portal capture options depending on different recording option (such as recording/replay).\n");
    printf("        Optional, set to \"$XDG_CONFIG_HOME/gpu-screen-recorder/restore_token\" by default ($XDG_CONFIG_HOME defaults to \"$HOME/.config\").\n");
    printf("        Note: the directory to the portal session token file is created automatically if it doesn't exist.\n");
    printf("\n");
    printf("  -encoder\n");
    printf("        Which device should be used for video encoding. Should either be 'gpu' or 'cpu'. 'cpu' option currently only work with h264 codec option (-k).\n");
    printf("        Optional, set to 'gpu' by default.\n");
    printf("\n");
    printf("  --info\n");
    printf("        List info about the system. Lists the following information (prints them to stdout and exits):\n");
    printf("        Supported video codecs (h264, h264_software, hevc, hevc_hdr, hevc_10bit, av1, av1_hdr, av1_10bit, vp8, vp9) and image codecs (jpeg, png) (if supported).\n");
    printf("        Supported capture options (window, focused, screen, monitors and portal, if supported by the system).\n");
    printf("        If opengl initialization fails then the program exits with 22, if no usable drm device is found then it exits with 23. On success it exits with 0.\n");
    printf("\n");
    printf("  --list-capture-options\n");
    printf("        List available capture options. Lists capture options in the following format (prints them to stdout and exits):\n");
    printf("          <option>\n");
    printf("          <monitor_name>|<resolution>\n");
    printf("        For example:\n");
    printf("          window\n");
    printf("          DP-1|1920x1080\n");
    printf("        The <option> and <monitor_name> is the name that can be passed to GPU Screen Recorder with the -w option.\n");
    printf("        --list-capture-options optionally accepts a card path (\"/dev/dri/cardN\") which can improve the performance of running this command.\n");
    printf("\n");
    printf("  --list-audio-devices\n");
    printf("        List audio devices. Lists audio devices in the following format (prints them to stdout and exits):\n");
    printf("          <audio_device_name>|<audio_device_name_in_human_readable_format>\n");
    printf("        For example:\n");
    printf("          bluez_input.88:C9:E8:66:A2:27|WH-1000XM4\n");
    printf("          alsa_output.pci-0000_0c_00.4.iec958-stereo|Monitor of Starship/Matisse HD Audio Controller Digital Stereo (IEC958)\n");
    printf("        The <audio_device_name> is the name that can be passed to GPU Screen Recorder with the -a option.\n");
    printf("\n");
    printf("  --list-application-audio\n");
    printf("        Lists applications that you can record from (prints them to stdout and exits), for example:\n");
    printf("          firefox\n");
    printf("          csgo\n");
    printf("        These names are the application audio names that can be passed to GPU Screen Recorder with the -a option.\n");
    printf("\n");
    printf("  --version\n");
    printf("        Print version (%s) and exit\n", GSR_VERSION);
    printf("\n");
    //fprintf(stderr, "  -pixfmt  The pixel format to use for the output video. yuv420 is the most common format and is best supported, but the color is compressed, so colors can look washed out and certain colors of text can look bad. Use yuv444 for no color compression, but the video may not work everywhere and it may not work with hardware video decoding. Optional, set to 'yuv420' by default\n");
    printf("  -o    The output file path. If omitted then the encoded data is sent to stdout. Required in replay mode (when using -r).\n");
    printf("        In replay mode this has to be a directory instead of a file.\n");
    printf("        Note: the directory to the file is created automatically if it doesn't already exist.\n");
    printf("\n");
    printf("  -ro   The output directory for regular recordings in replay/streaming mode. Required to start recording in replay/streaming mode.\n");
    printf("        Note: the directory to the file is created automatically if it doesn't already exist.\n");
    printf("\n");
    printf("  -v    Prints fps and damage info once per second. Optional, set to 'yes' by default.\n");
    printf("\n");
    printf("  -gl-debug\n");
    printf("        Print opengl debug output. Optional, set to 'no' by default.\n");
    printf("\n");
    printf("  -h, --help\n");
    printf("        Show this help.\n");
    printf("\n");
    printf("NOTES:\n");
    printf("  Send signal SIGINT to gpu-screen-recorder (Ctrl+C, or pkill -SIGINT -f gpu-screen-recorder) to stop and save the recording. When in replay mode this stops recording without saving.\n");
    printf("  Send signal SIGUSR2 to gpu-screen-recorder (pkill -SIGUSR2 -f gpu-screen-recorder) to pause/unpause recording. Only applicable when recording (not streaming nor replay).\n");
    printf("  Send signal SIGUSR1 to gpu-screen-recorder (pkill -SIGUSR1 -f gpu-screen-recorder) to save a replay (when in replay mode).\n");
    printf("  Send signal SIGRTMIN+1 to gpu-screen-recorder (pkill -SIGRTMIN+1 -f gpu-screen-recorder) to save a replay of the last 10 seconds (when in replay mode).\n");
    printf("  Send signal SIGRTMIN+2 to gpu-screen-recorder (pkill -SIGRTMIN+2 -f gpu-screen-recorder) to save a replay of the last 30 seconds (when in replay mode).\n");
    printf("  Send signal SIGRTMIN+3 to gpu-screen-recorder (pkill -SIGRTMIN+3 -f gpu-screen-recorder) to save a replay of the last 60 seconds (when in replay mode).\n");
    printf("  Send signal SIGRTMIN+4 to gpu-screen-recorder (pkill -SIGRTMIN+4 -f gpu-screen-recorder) to save a replay of the last 5 minutes (when in replay mode).\n");
    printf("  Send signal SIGRTMIN+5 to gpu-screen-recorder (pkill -SIGRTMIN+5 -f gpu-screen-recorder) to save a replay of the last 10 minutes (when in replay mode).\n");
    printf("  Send signal SIGRTMIN+6 to gpu-screen-recorder (pkill -SIGRTMIN+6 -f gpu-screen-recorder) to save a replay of the last 30 minutes (when in replay mode).\n");
    printf("  Send signal SIGRTMIN to gpu-screen-recorder (pkill -SIGRTMIN -f gpu-screen-recorder) to start/stop recording a regular video when in replay/streaming mode.\n");
    printf("\n");
    printf("EXAMPLES:\n");
    printf("  %s -w screen -f 60 -a default_output -o video.mp4\n", program_name);
    printf("  %s -w screen -f 60 -a default_output -a default_input -o video.mp4\n", program_name);
    printf("  %s -w screen -f 60 -a \"default_output|default_input\" -o video.mp4\n", program_name);
    printf("  %s -w screen -f 60 -a default_output -c mkv -r 60 -o \"$HOME/Videos\"\n", program_name);
    printf("  %s -w screen -f 60 -a default_output -c mkv -sc script.sh -r 60 -o \"$HOME/Videos\"\n", program_name);
    printf("  %s -w portal -f 60 -a default_output -restore-portal-session yes -o video.mp4\n", program_name);
    printf("  %s -w screen -f 60 -a default_output -bm cbr -q 15000 -o video.mp4\n", program_name);
    printf("  %s -w screen -f 60 -a \"app:firefox|app:csgo\" -o video.mp4\n", program_name);
    printf("  %s -w screen -f 60 -a \"app-inverse:firefox|app-inverse:csgo\" -o video.mp4\n", program_name);
    printf("  %s -w screen -f 60 -a \"default_input|app-inverse:Brave\" -o video.mp4\n", program_name);
    printf("  %s -w screen -o image.jpg\n", program_name);
    printf("  %s -w screen -q medium -o image.jpg\n", program_name);
    printf("  %s -w region -region 640x480+100+100 -o video.mp4\n", program_name);
    printf("  %s -w region -region $(slop) -o video.mp4\n", program_name);
    printf("  %s -w region -region $(slurp -f \"%%wx%%h+%%x+%%y\") -o video.mp4\n", program_name);
    //fprintf(stderr, "  gpu-screen-recorder -w screen -f 60 -q ultra -pixfmt yuv444 -o video.mp4\n");
    fflush(stdout);
}

static void usage() {
    usage_header();
}

// TODO: Does this match all livestreaming cases?
static bool is_livestream_path(const char *str) {
    const int len = strlen(str);
    if((len >= 7 && memcmp(str, "http://", 7) == 0) || (len >= 8 && memcmp(str, "https://", 8) == 0))
        return true;
    else if((len >= 7 && memcmp(str, "rtmp://", 7) == 0) || (len >= 8 && memcmp(str, "rtmps://", 8) == 0))
        return true;
    else if((len >= 7 && memcmp(str, "rtsp://", 7) == 0))
        return true;
    else if((len >= 6 && memcmp(str, "srt://", 6) == 0))
        return true;
    else if((len >= 6 && memcmp(str, "tcp://", 6) == 0))
        return true;
    else if((len >= 6 && memcmp(str, "udp://", 6) == 0))
        return true;
    else
        return false;
}

static bool args_parser_set_values(args_parser *self) {
    self->video_encoder = (gsr_video_encoder_hardware)args_get_enum_by_key(self->args, NUM_ARGS, "-encoder", GSR_VIDEO_ENCODER_HW_GPU);
    self->pixel_format = (gsr_pixel_format)args_get_enum_by_key(self->args, NUM_ARGS, "-pixfmt", GSR_PIXEL_FORMAT_YUV420);
    self->framerate_mode = (gsr_framerate_mode)args_get_enum_by_key(self->args, NUM_ARGS, "-fm", GSR_FRAMERATE_MODE_VARIABLE);
    self->color_range = (gsr_color_range)args_get_enum_by_key(self->args, NUM_ARGS, "-cr", GSR_COLOR_RANGE_LIMITED);
    self->tune = (gsr_tune)args_get_enum_by_key(self->args, NUM_ARGS, "-tune", GSR_TUNE_PERFORMANCE);
    self->video_codec = (gsr_video_codec)args_get_enum_by_key(self->args, NUM_ARGS, "-k", GSR_VIDEO_CODEC_AUTO);
    self->audio_codec = (gsr_audio_codec)args_get_enum_by_key(self->args, NUM_ARGS, "-ac", GSR_AUDIO_CODEC_OPUS);
    self->bitrate_mode = (gsr_bitrate_mode)args_get_enum_by_key(self->args, NUM_ARGS, "-bm", GSR_BITRATE_MODE_AUTO);

    const char *window = args_get_value_by_key(self->args, NUM_ARGS, "-w");
    snprintf(self->window, sizeof(self->window), "%s", window);
    self->verbose = args_get_boolean_by_key(self->args, NUM_ARGS, "-v", true);
    self->gl_debug = args_get_boolean_by_key(self->args, NUM_ARGS, "-gl-debug", false);
    self->record_cursor = args_get_boolean_by_key(self->args, NUM_ARGS, "-cursor", true);
    self->date_folders = args_get_boolean_by_key(self->args, NUM_ARGS, "-df", false);
    self->restore_portal_session = args_get_boolean_by_key(self->args, NUM_ARGS, "-restore-portal-session", false);
    self->restart_replay_on_save = args_get_boolean_by_key(self->args, NUM_ARGS, "-restart-replay-on-save", false);
    self->overclock = args_get_boolean_by_key(self->args, NUM_ARGS, "-oc", false);

    self->audio_bitrate = args_get_i64_by_key(self->args, NUM_ARGS, "-ab", 0);
    self->audio_bitrate *= 1000LL;

    self->keyint = args_get_double_by_key(self->args, NUM_ARGS, "-keyint", 2.0);

    if(self->audio_codec == GSR_AUDIO_CODEC_FLAC) {
        fprintf(stderr, "Warning: flac audio codec is temporary disabled, using opus audio codec instead\n");
        self->audio_codec = GSR_AUDIO_CODEC_OPUS;
    }

    self->portal_session_token_filepath = args_get_value_by_key(self->args, NUM_ARGS, "-portal-session-token-filepath");
    if(self->portal_session_token_filepath) {
        int len = strlen(self->portal_session_token_filepath);
        if(len > 0 && self->portal_session_token_filepath[len - 1] == '/') {
            fprintf(stderr, "Error: -portal-session-token-filepath should be a path to a file but it ends with a /: %s\n", self->portal_session_token_filepath);
            return false;
        }
    }

    self->recording_saved_script = args_get_value_by_key(self->args, NUM_ARGS, "-sc");
    if(self->recording_saved_script) {
        struct stat buf;
        if(stat(self->recording_saved_script, &buf) == -1 || !S_ISREG(buf.st_mode)) {
            fprintf(stderr, "Error: Script \"%s\" either doesn't exist or it's not a file\n", self->recording_saved_script);
            usage();
            return false;
        }

        if(!(buf.st_mode & S_IXUSR)) {
            fprintf(stderr, "Error: Script \"%s\" is not executable\n", self->recording_saved_script);
            usage();
            return false;
        }
    }

    const char *quality_str = args_get_value_by_key(self->args, NUM_ARGS, "-q");
    self->video_quality = GSR_VIDEO_QUALITY_VERY_HIGH;
    self->video_bitrate = 0;

    if(self->bitrate_mode == GSR_BITRATE_MODE_CBR) {
        if(!quality_str) {
            fprintf(stderr, "Error: option '-q' is required when using '-bm cbr' option\n");
            usage();
            return false;
        }

        if(sscanf(quality_str, "%" PRIi64, &self->video_bitrate) != 1) {
            fprintf(stderr, "Error: -q argument \"%s\" is not an integer value. When using '-bm cbr' option '-q' is expected to be an integer value\n", quality_str);
            usage();
            return false;
        }

        if(self->video_bitrate < 0) {
            fprintf(stderr, "Error: -q is expected to be 0 or larger, got %" PRIi64 "\n", self->video_bitrate);
            usage();
            return false;
        }

        self->video_bitrate *= 1000LL;
    } else {
        if(!quality_str)
            quality_str = "very_high";

        if(strcmp(quality_str, "medium") == 0) {
            self->video_quality = GSR_VIDEO_QUALITY_MEDIUM;
        } else if(strcmp(quality_str, "high") == 0) {
            self->video_quality = GSR_VIDEO_QUALITY_HIGH;
        } else if(strcmp(quality_str, "very_high") == 0) {
            self->video_quality = GSR_VIDEO_QUALITY_VERY_HIGH;
        } else if(strcmp(quality_str, "ultra") == 0) {
            self->video_quality = GSR_VIDEO_QUALITY_ULTRA;
        } else {
            fprintf(stderr, "Error: -q should either be 'medium', 'high', 'very_high' or 'ultra', got: '%s'\n", quality_str);
            usage();
            return false;
        }
    }

    const char *output_resolution_str = args_get_value_by_key(self->args, NUM_ARGS, "-s");
    if(!output_resolution_str && strcmp(self->window, "focused") == 0) {
        fprintf(stderr, "Error: option -s is required when using '-w focused' option\n");
        usage();
        return false;
    }

    self->output_resolution = (vec2i){0, 0};
    if(output_resolution_str) {
        if(sscanf(output_resolution_str, "%dx%d", &self->output_resolution.x, &self->output_resolution.y) != 2) {
            fprintf(stderr, "Error: invalid value for option -s '%s', expected a value in format WxH\n", output_resolution_str);
            usage();
            return false;
        }

        if(self->output_resolution.x < 0 || self->output_resolution.y < 0) {
            fprintf(stderr, "Error: invalid value for option -s '%s', expected width and height to be greater or equal to 0\n", output_resolution_str);
            usage();
            return false;
        }
    }

    self->region_size = (vec2i){0, 0};
    self->region_position = (vec2i){0, 0};
    const char *region_str = args_get_value_by_key(self->args, NUM_ARGS, "-region");
    if(region_str) {
        if(strcmp(self->window, "region") != 0) {
            fprintf(stderr, "Error: option -region can only be used when option '-w region' is used\n");
            usage();
            return false;
        }

        if(sscanf(region_str, "%dx%d+%d+%d", &self->region_size.x, &self->region_size.y, &self->region_position.x, &self->region_position.y) != 4) {
            fprintf(stderr, "Error: invalid value for option -region '%s', expected a value in format WxH+X+Y\n", region_str);
            usage();
            return false;
        }

        if(self->region_size.x < 0 || self->region_size.y < 0 || self->region_position.x < 0 || self->region_position.y < 0) {
            fprintf(stderr, "Error: invalid value for option -region '%s', expected width, height, x and y to be greater or equal to 0\n", region_str);
            usage();
            return false;
        }
    } else {
        if(strcmp(self->window, "region") == 0) {
            fprintf(stderr, "Error: option -region is required when '-w region' is used\n");
            usage();
            return false;
        }
    }

    self->fps = args_get_i64_by_key(self->args, NUM_ARGS, "-f", 60);
    self->replay_buffer_size_secs = args_get_i64_by_key(self->args, NUM_ARGS, "-r", -1);
    if(self->replay_buffer_size_secs != -1)
        self->replay_buffer_size_secs += (int64_t)(self->keyint + 0.5); // Add a few seconds to account of lost packets because of non-keyframe packets skipped

    self->container_format = args_get_value_by_key(self->args, NUM_ARGS, "-c");
    if(self->container_format && strcmp(self->container_format, "mkv") == 0)
        self->container_format = "matroska";

    const bool is_replaying = self->replay_buffer_size_secs != -1;
    self->is_livestream = false;
    self->filename = args_get_value_by_key(self->args, NUM_ARGS, "-o");
    if(self->filename) {
        self->is_livestream = is_livestream_path(self->filename);
        if(self->is_livestream) {
            if(is_replaying) {
                fprintf(stderr, "Error: replay mode is not applicable to live streaming\n");
                return false;
            }
        } else {
            if(!is_replaying) {
                char directory_buf[PATH_MAX];
                snprintf(directory_buf, sizeof(directory_buf), "%s", self->filename);
                char *directory = dirname(directory_buf);
                if(strcmp(directory, ".") != 0 && strcmp(directory, "/") != 0) {
                    if(create_directory_recursive(directory) != 0) {
                        fprintf(stderr, "Error: failed to create directory for output file: %s\n", self->filename);
                        return false;
                    }
                }
            } else {
                if(!self->container_format) {
                    fprintf(stderr, "Error: option -c is required when using option -r\n");
                    usage();
                    return false;
                }

                struct stat buf;
                if(stat(self->filename, &buf) != -1 && !S_ISDIR(buf.st_mode)) {
                    fprintf(stderr, "Error: File \"%s\" exists but it's not a directory\n", self->filename);
                    usage();
                    return false;
                }
            }
        }
    } else {
        if(!is_replaying) {
            self->filename = "/dev/stdout";
        } else {
            fprintf(stderr, "Error: Option -o is required when using option -r\n");
            usage();
            return false;
        }

        if(!self->container_format) {
            fprintf(stderr, "Error: option -c is required when not using option -o\n");
            usage();
            return false;
        }
    }

    self->is_output_piped = strcmp(self->filename, "/dev/stdout") == 0;
    self->low_latency_recording = self->is_livestream || self->is_output_piped;

    self->replay_recording_directory = args_get_value_by_key(self->args, NUM_ARGS, "-ro");

    const bool is_portal_capture = strcmp(self->window, "portal") == 0;
    if(!self->restore_portal_session && is_portal_capture)
        fprintf(stderr, "Info: option '-w portal' was used without '-restore-portal-session yes'. The previous screencast session will be ignored\n");

    if(self->is_livestream && self->recording_saved_script) {
        fprintf(stderr, "Warning: live stream detected, -sc script is ignored\n");
        self->recording_saved_script = NULL;
    }

    return true;
}

bool args_parser_parse(args_parser *self, int argc, char **argv, const args_handlers *arg_handlers, void *userdata) {
    assert(arg_handlers);
    memset(self, 0, sizeof(*self));

    if(argc <= 1) {
        usage_full();
        return false;
    }

    if(argc == 2 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        usage_full();
        return false;
    }

    if(argc == 2 && strcmp(argv[1], "--info") == 0) {
        arg_handlers->info(userdata);
        return true;
    }

    if(argc == 2 && strcmp(argv[1], "--list-audio-devices") == 0) {
        arg_handlers->list_audio_devices(userdata);
        return true;
    }

    if(argc == 2 && strcmp(argv[1], "--list-application-audio") == 0) {
        arg_handlers->list_application_audio(userdata);
        return true;
    }

    if(strcmp(argv[1], "--list-capture-options") == 0) {
        if(argc == 2) {
            arg_handlers->list_capture_options(NULL, userdata);
            return true;
        } else if(argc == 3 || argc == 4) {
            const char *card_path = argv[2];
            arg_handlers->list_capture_options(card_path, userdata);
            return true;
        } else {
            fprintf(stderr, "Error: expected --list-capture-options to be called with either no extra arguments or 1 extra argument (card path)\n");
            return false;
        }
    }

    if(argc == 2 && strcmp(argv[1], "--version") == 0) {
        arg_handlers->version(userdata);
        return true;
    }

    int arg_index = 0;
    self->args[arg_index++] = (Arg){ .key = "-w",                             .optional = false, .list = false, .type = ARG_TYPE_STRING  };
    self->args[arg_index++] = (Arg){ .key = "-c",                             .optional = true,  .list = false, .type = ARG_TYPE_STRING  };
    self->args[arg_index++] = (Arg){ .key = "-f",                             .optional = true,  .list = false, .type = ARG_TYPE_I64, .integer_value_min = 1, .integer_value_max = 1000 };
    self->args[arg_index++] = (Arg){ .key = "-s",                             .optional = true,  .list = false, .type = ARG_TYPE_STRING  };
    self->args[arg_index++] = (Arg){ .key = "-region",                        .optional = true,  .list = false, .type = ARG_TYPE_STRING  };
    self->args[arg_index++] = (Arg){ .key = "-a",                             .optional = true,  .list = true,  .type = ARG_TYPE_STRING  };
    self->args[arg_index++] = (Arg){ .key = "-q",                             .optional = true,  .list = false, .type = ARG_TYPE_STRING  };
    self->args[arg_index++] = (Arg){ .key = "-o",                             .optional = true,  .list = false, .type = ARG_TYPE_STRING  };
    self->args[arg_index++] = (Arg){ .key = "-ro",                            .optional = true,  .list = false, .type = ARG_TYPE_STRING  };
    self->args[arg_index++] = (Arg){ .key = "-r",                             .optional = true,  .list = false, .type = ARG_TYPE_I64, .integer_value_min = 2, .integer_value_max = 10800 };
    self->args[arg_index++] = (Arg){ .key = "-restart-replay-on-save",        .optional = true,  .list = false, .type = ARG_TYPE_BOOLEAN };
    self->args[arg_index++] = (Arg){ .key = "-k",                             .optional = true,  .list = false, .type = ARG_TYPE_ENUM, .enum_values = video_codec_enums, .num_enum_values = sizeof(video_codec_enums)/sizeof(ArgEnum) };
    self->args[arg_index++] = (Arg){ .key = "-ac",                            .optional = true,  .list = false, .type = ARG_TYPE_ENUM, .enum_values = audio_codec_enums, .num_enum_values = sizeof(audio_codec_enums)/sizeof(ArgEnum) };
    self->args[arg_index++] = (Arg){ .key = "-ab",                            .optional = true,  .list = false, .type = ARG_TYPE_I64, .integer_value_min = 0, .integer_value_max = 50000 };
    self->args[arg_index++] = (Arg){ .key = "-oc",                            .optional = true,  .list = false, .type = ARG_TYPE_BOOLEAN };
    self->args[arg_index++] = (Arg){ .key = "-fm",                            .optional = true,  .list = false, .type = ARG_TYPE_ENUM, .enum_values = framerate_mode_enums, .num_enum_values = sizeof(framerate_mode_enums)/sizeof(ArgEnum) };
    self->args[arg_index++] = (Arg){ .key = "-bm",                            .optional = true,  .list = false, .type = ARG_TYPE_ENUM, .enum_values = bitrate_mode_enums, .num_enum_values = sizeof(bitrate_mode_enums)/sizeof(ArgEnum) };
    self->args[arg_index++] = (Arg){ .key = "-pixfmt",                        .optional = true,  .list = false, .type = ARG_TYPE_ENUM, .enum_values = pixel_format_enums, .num_enum_values = sizeof(pixel_format_enums)/sizeof(ArgEnum) };
    self->args[arg_index++] = (Arg){ .key = "-v",                             .optional = true,  .list = false, .type = ARG_TYPE_BOOLEAN };
    self->args[arg_index++] = (Arg){ .key = "-gl-debug",                      .optional = true,  .list = false, .type = ARG_TYPE_BOOLEAN };
    self->args[arg_index++] = (Arg){ .key = "-df",                            .optional = true,  .list = false, .type = ARG_TYPE_BOOLEAN };
    self->args[arg_index++] = (Arg){ .key = "-sc",                            .optional = true,  .list = false, .type = ARG_TYPE_STRING  };
    self->args[arg_index++] = (Arg){ .key = "-cr",                            .optional = true,  .list = false, .type = ARG_TYPE_ENUM, .enum_values = color_range_enums, .num_enum_values = sizeof(color_range_enums)/sizeof(ArgEnum) };
    self->args[arg_index++] = (Arg){ .key = "-tune",                          .optional = true,  .list = false, .type = ARG_TYPE_ENUM, .enum_values = tune_enums, .num_enum_values = sizeof(tune_enums)/sizeof(ArgEnum) };
    self->args[arg_index++] = (Arg){ .key = "-cursor",                        .optional = true,  .list = false, .type = ARG_TYPE_BOOLEAN };
    self->args[arg_index++] = (Arg){ .key = "-keyint",                        .optional = true,  .list = false, .type = ARG_TYPE_DOUBLE, .integer_value_min = 0, .integer_value_max = 500 };
    self->args[arg_index++] = (Arg){ .key = "-restore-portal-session",        .optional = true,  .list = false, .type = ARG_TYPE_BOOLEAN };
    self->args[arg_index++] = (Arg){ .key = "-portal-session-token-filepath", .optional = true,  .list = false, .type = ARG_TYPE_STRING  };
    self->args[arg_index++] = (Arg){ .key = "-encoder",                       .optional = true,  .list = false, .type = ARG_TYPE_ENUM, .enum_values = video_encoder_enums, .num_enum_values = sizeof(video_encoder_enums)/sizeof(ArgEnum) };
    assert(arg_index == NUM_ARGS);

    for(int i = 1; i < argc; i += 2) {
        const char *arg_name = argv[i];
        Arg *arg = args_get_by_key(self->args, NUM_ARGS, arg_name);
        if(!arg) {
            fprintf(stderr, "Error: invalid argument '%s'\n", arg_name);
            usage();
            return false;
        }

        if(arg->num_values > 0 && !arg->list) {
            fprintf(stderr, "Error: expected argument '%s' to only be specified once\n", arg_name);
            usage();
            return false;
        }

        if(i + 1 >= argc) {
            fprintf(stderr, "Error: missing value for argument '%s'\n", arg_name);
            usage();
            return false;
        }

        const char *arg_value = argv[i + 1];
        switch(arg->type) {
            case ARG_TYPE_STRING: {
                break;
            }
            case ARG_TYPE_BOOLEAN: {
                if(strcmp(arg_value, "yes") == 0) {
                    arg->typed_value.boolean = true;
                } else if(strcmp(arg_value, "no") == 0) {
                    arg->typed_value.boolean = false;
                } else {
                    fprintf(stderr, "Error: %s should either be 'yes' or 'no', got: '%s'\n", arg_name, arg_value);
                    usage();
                    return false;
                }
                break;
            }
            case ARG_TYPE_ENUM: {
                if(!arg_get_enum_value_by_name(arg, arg_value, &arg->typed_value.enum_value)) {
                    fprintf(stderr, "Error: %s should either be ", arg_name);
                    arg_print_expected_enum_names(arg);
                    fprintf(stderr, ", got: '%s'\n", arg_value);
                    usage();
                    return false;
                }
                break;
            }
            case ARG_TYPE_I64: {
                if(sscanf(arg_value, "%" PRIi64, &arg->typed_value.i64_value) != 1) {
                    fprintf(stderr, "Error: %s argument \"%s\" is not an integer\n", arg_name, arg_value);
                    usage();
                    return false;
                }

                if(arg->typed_value.i64_value < arg->integer_value_min) {
                    fprintf(stderr, "Error: %s argument is expected to be larger than %" PRIi64 ", got %" PRIi64 "\n", arg_name, arg->integer_value_min, arg->typed_value.i64_value);
                    usage();
                    return false;
                }

                if(arg->typed_value.i64_value > arg->integer_value_max) {
                    fprintf(stderr, "Error: %s argument is expected to be less than %" PRIi64 ", got %" PRIi64 "\n", arg_name, arg->integer_value_max, arg->typed_value.i64_value);
                    usage();
                    return false;
                }
                break;
            }
            case ARG_TYPE_DOUBLE: {
                if(sscanf(arg_value, "%lf", &arg->typed_value.d_value) != 1) {
                    fprintf(stderr, "Error: %s argument \"%s\" is not an floating-point number\n", arg_name, arg_value);
                    usage();
                    return false;
                }

                if(arg->typed_value.d_value < arg->integer_value_min) {
                    fprintf(stderr, "Error: %s argument is expected to be larger than %" PRIi64 ", got %lf\n", arg_name, arg->integer_value_min, arg->typed_value.d_value);
                    usage();
                    return false;
                }

                if(arg->typed_value.d_value > arg->integer_value_max) {
                    fprintf(stderr, "Error: %s argument is expected to be less than %" PRIi64 ", got %lf\n", arg_name, arg->integer_value_max, arg->typed_value.d_value);
                    usage();
                    return false;
                }
                break;
            }
        }

        if(!arg_append_value(arg, arg_value)) {
            fprintf(stderr, "Error: failed to append argument, out of memory\n");
            return false;
        }
    }

    for(int i = 0; i < NUM_ARGS; ++i) {
        const Arg *arg = &self->args[i];
        if(!arg->optional && arg->num_values == 0) {
            fprintf(stderr, "Error: missing argument '%s'\n", arg->key);
            usage();
            return false;
        }
    }

    return args_parser_set_values(self);
}

void args_parser_deinit(args_parser *self) {
    for(int i = 0; i < NUM_ARGS; ++i) {
        arg_deinit(&self->args[i]);
    }
}

bool args_parser_validate_with_gl_info(args_parser *self, gsr_egl *egl) {
    const bool wayland = gsr_window_get_display_server(egl->window) == GSR_DISPLAY_SERVER_WAYLAND;

    if(self->bitrate_mode == (gsr_bitrate_mode)GSR_BITRATE_MODE_AUTO) {
        // QP is broken on steam deck, see https://github.com/ValveSoftware/SteamOS/issues/1609
        self->bitrate_mode = egl->gpu_info.is_steam_deck ? GSR_BITRATE_MODE_VBR : GSR_BITRATE_MODE_QP;
    }

    if(egl->gpu_info.is_steam_deck && self->bitrate_mode == GSR_BITRATE_MODE_QP) {
        fprintf(stderr, "Warning: qp bitrate mode is not supported on Steam Deck because of Steam Deck driver bugs. Using vbr instead\n");
        self->bitrate_mode = GSR_BITRATE_MODE_VBR;
    }

    if(self->video_encoder == GSR_VIDEO_ENCODER_HW_CPU && self->bitrate_mode == GSR_BITRATE_MODE_VBR) {
        fprintf(stderr, "Warning: bitrate mode has been forcefully set to qp because software encoding option doesn't support vbr option\n");
        self->bitrate_mode = GSR_BITRATE_MODE_QP;
    }

    if(egl->gpu_info.vendor != GSR_GPU_VENDOR_NVIDIA && self->overclock) {
        fprintf(stderr, "Info: overclock option has no effect on amd/intel, ignoring option\n");
        self->overclock = false;
    }

    if(egl->gpu_info.vendor == GSR_GPU_VENDOR_NVIDIA && self->overclock && wayland) {
        fprintf(stderr, "Info: overclocking is not possible on nvidia on wayland, ignoring option\n");
        self->overclock = false;
    }

    if(egl->gpu_info.is_steam_deck) {
        fprintf(stderr, "Warning: steam deck has multiple driver issues. One of them has been reported here: https://github.com/ValveSoftware/SteamOS/issues/1609\n"
            "If you have issues with GPU Screen Recorder on steam deck that you don't have on a desktop computer then report the issue to Valve and/or AMD.\n");
    }

    self->very_old_gpu = false;
    if(egl->gpu_info.vendor == GSR_GPU_VENDOR_NVIDIA && egl->gpu_info.gpu_version != 0 && egl->gpu_info.gpu_version < 900) {
        fprintf(stderr, "Info: your gpu appears to be very old (older than maxwell architecture). Switching to lower preset\n");
        self->very_old_gpu = true;
    }

    if(video_codec_is_hdr(self->video_codec) && !wayland) {
        fprintf(stderr, "Error: hdr video codec option %s is not available on X11\n", video_codec_to_string(self->video_codec));
        usage();
        return false;
    }

    const bool is_portal_capture = strcmp(self->window, "portal") == 0;
    if(video_codec_is_hdr(self->video_codec) && is_portal_capture) {
        fprintf(stderr, "Warning: portal capture option doesn't support hdr yet (PipeWire doesn't support hdr), the video will be tonemapped from hdr to sdr\n");
        self->video_codec = hdr_video_codec_to_sdr_video_codec(self->video_codec);
    }

    return true;
}

void args_parser_print_usage(void) {
    usage();
}

Arg* args_parser_get_arg(args_parser *self, const char *arg_name) {
    return args_get_by_key(self->args, NUM_ARGS, arg_name);
}
