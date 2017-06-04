/*
Copyright (c) 2013, Broadcom Europe Ltd
Copyright (c) 2013, Silvan Melchior
Copyright (c) 2013, James Hughes
Copyright (c) 2015, Frank Hunleth
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the copyright holder nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

/**
 * \file raspijpgs.c
 * Command line program to capture and stream MJPEG video on a Raspberry Pi.
 *
 * Description
 *
 * Raspijpgs is a Unix commandline-friendly MJPEG streaming program with parts
 * copied from RaspiMJPEG, RaspiVid, and RaspiStill. It can be run as either
 * a client or server. The server connects to the Pi Camera via the MMAL
 * interface. It can either record video itself or send it to clients. All
 * interprocess communication is done via Unix Domain sockets.
 *
 * For usage and examples, see README.md
 */

#define _GNU_SOURCE // for asprintf()
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <memory.h>
#include <math.h>
#include <signal.h>
#include <fcntl.h>
#include <time.h>
#include <err.h>
#include <ctype.h>
#include <poll.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <arpa/inet.h> // for ntohl

#include "bcm_host.h"
#include "interface/vcos/vcos.h"
#include "interface/mmal/mmal.h"
#include "interface/mmal/mmal_buffer.h"
#include "interface/mmal/util/mmal_util.h"
#include "interface/mmal/util/mmal_util_params.h"
#include "interface/mmal/util/mmal_default_components.h"
#include "interface/mmal/util/mmal_connection.h"
#include "interface/mmal/mmal_parameters_camera.h"

#define MAX_DATA_BUFFER_SIZE        131072
#define MAX_REQUEST_BUFFER_SIZE     4096

#define UNUSED(expr) do { (void)(expr); } while (0)

// Environment config keys
#define RASPIJPGS_WIDTH             "RASPIJPGS_WIDTH"
#define RASPIJPGS_HEIGHT            "RASPIJPGS_HEIGHT"
#define RASPIJPGS_FPS		    "RASPIJPGS_FPS"
#define RASPIJPGS_ANNOTATION        "RASPIJPGS_ANNOTATION"
#define RASPIJPGS_ANNO_BACKGROUND   "RASPIJPGS_ANNO_BACKGROUND"
#define RASPIJPGS_SHARPNESS         "RASPIJPGS_SHARPNESS"
#define RASPIJPGS_CONTRAST          "RASPIJPGS_CONTRAST"
#define RASPIJPGS_BRIGHTNESS        "RASPIJPGS_BRIGHTNESS"
#define RASPIJPGS_SATURATION        "RASPIJPGS_SATURATION"
#define RASPIJPGS_ISO               "RASPIJPGS_ISO"
#define RASPIJPGS_VSTAB             "RASPIJPGS_VSTAB"
#define RASPIJPGS_EV                "RASPIJPGS_EV"
#define RASPIJPGS_EXPOSURE          "RASPIJPGS_EXPOSURE"
#define RASPIJPGS_AWB               "RASPIJPGS_AWB"
#define RASPIJPGS_IMXFX             "RASPIJPGS_IMXFX"
#define RASPIJPGS_COLFX             "RASPIJPGS_COLFX"
#define RASPIJPGS_SENSOR_MODE       "RASPIJPGS_SENSOR_MODE"
#define RASPIJPGS_METERING          "RASPIJPGS_METERING"
#define RASPIJPGS_ROTATION          "RASPIJPGS_ROTATION"
#define RASPIJPGS_HFLIP             "RASPIJPGS_HFLIP"
#define RASPIJPGS_VFLIP             "RASPIJPGS_VFLIP"
#define RASPIJPGS_ROI               "RASPIJPGS_ROI"
#define RASPIJPGS_SHUTTER           "RASPIJPGS_SHUTTER"
#define RASPIJPGS_QUALITY           "RASPIJPGS_QUALITY"
#define RASPIJPGS_RESTART_INTERVAL  "RASPIJPGS_RESTART_INTERVAL"

// Globals

enum config_context {
    config_context_parse_cmdline,
    config_context_file,
    config_context_server_start,
    config_context_client_request
};

struct raspijpgs_state
{
    // Settings
    char *config_filename;
    char *sendlist;

    // Communication
    char *socket_buffer;
    int socket_buffer_ix;
    char *stdin_buffer;
    int stdin_buffer_ix;

    // MMAL resources
    MMAL_COMPONENT_T *camera;
    MMAL_COMPONENT_T *jpegencoder;
    MMAL_COMPONENT_T *resizer;
    MMAL_CONNECTION_T *con_cam_res;
    MMAL_CONNECTION_T *con_res_jpeg;
    MMAL_POOL_T *pool_jpegencoder;

    // MMAL callback -> main loop
    int mmal_callback_pipe[2];
};

static struct raspijpgs_state state = {0};

struct raspi_config_opt
{
    const char *long_option;
    const char *short_option;
    const char *env_key;
    const char *help;

    const char *default_value;

    // Record the value (called as options are set)
    // Set replace=0 to only set the value if it hasn't been set already.
    void (*set)(const struct raspi_config_opt *, const char *value, enum config_context context);

    // Apply the option (called on every option)
    void (*apply)(const struct raspi_config_opt *, enum config_context context);
};
static struct raspi_config_opt opts[];

static void default_set(const struct raspi_config_opt *opt, const char *value, enum config_context context)
{
    if (!opt->env_key)
        return;

    // setenv's 3rd parameter is whether to replace a value if it already
    // exists. Sets are done in the order of Environment, commandline, file.
    // Since the file should be the lowest priority, set it to not replace
    // here.
    int replace = (context != config_context_file);

    if (value) {
        if (setenv(opt->env_key, value, replace) < 0)
            err(EXIT_FAILURE, "Error setting %s to %s", opt->env_key, opt->default_value);
    } else {
        if (replace && (unsetenv(opt->env_key) < 0))
            err(EXIT_FAILURE, "Error unsetting %s", opt->env_key);
    }
}

static void setstring(char **left, const char *right)
{
    if (*left)
        free(*left);
    *left = strdup(right);
}

static int constrain(int minimum, int value, int maximum)
{
    if (value < minimum)
        return minimum;
    else if (value > maximum)
        return maximum;
    else
        return value;
}

static void config_set(const struct raspi_config_opt *opt, const char *value, enum config_context context)
{
    UNUSED(opt); UNUSED(context);
    setstring(&state.config_filename, value);
}

static void help(const struct raspi_config_opt *opt, const char *value, enum config_context context);

static void width_apply(const struct raspi_config_opt *opt, enum config_context context) { UNUSED(opt); }
static void height_apply(const struct raspi_config_opt *opt, enum config_context context) { UNUSED(opt); }
static void annotation_apply(const struct raspi_config_opt *opt, enum config_context context) { UNUSED(opt); }
static void anno_background_apply(const struct raspi_config_opt *opt, enum config_context context) { UNUSED(opt); }
static void rational_param_apply(int mmal_param, const struct raspi_config_opt *opt, enum config_context context)
{
    unsigned int value = strtoul(getenv(opt->env_key), 0, 0);
    if (value > 100) {
        if (context == config_context_server_start)
            errx(EXIT_FAILURE, "%s must be between 0 and 100", opt->long_option);
        else
            return;
    }
    MMAL_RATIONAL_T mmal_value = {value, 100};
    MMAL_STATUS_T status = mmal_port_parameter_set_rational(state.camera->control, mmal_param, mmal_value);
    if(status != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not set %s", opt->long_option);
}

static void sharpness_apply(const struct raspi_config_opt *opt, enum config_context context)
{
    rational_param_apply(MMAL_PARAMETER_SHARPNESS, opt, context);
}
static void contrast_apply(const struct raspi_config_opt *opt, enum config_context context)
{
    rational_param_apply(MMAL_PARAMETER_CONTRAST, opt, context);
}
static void brightness_apply(const struct raspi_config_opt *opt, enum config_context context)
{
    rational_param_apply(MMAL_PARAMETER_BRIGHTNESS, opt, context);
}
static void saturation_apply(const struct raspi_config_opt *opt, enum config_context context)
{
    rational_param_apply(MMAL_PARAMETER_SATURATION, opt, context);
}

static void ISO_apply(const struct raspi_config_opt *opt, enum config_context context)
{
    UNUSED(context);
    unsigned int value = strtoul(getenv(opt->env_key), 0, 0);
    MMAL_STATUS_T status = mmal_port_parameter_set_uint32(state.camera->control, MMAL_PARAMETER_ISO, value);
    if(status != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not set %s", opt->long_option);
}
static void vstab_apply(const struct raspi_config_opt *opt, enum config_context context)
{
    UNUSED(context);
    unsigned int value = (strcmp(getenv(opt->env_key), "on") == 0);
    MMAL_STATUS_T status = mmal_port_parameter_set_uint32(state.camera->control, MMAL_PARAMETER_VIDEO_STABILISATION, value);
    if(status != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not set %s", opt->long_option);
}
static void ev_apply(const struct raspi_config_opt *opt, enum config_context context)
{
    // TODO
    UNUSED(opt);
    UNUSED(context);
}
static void exposure_apply(const struct raspi_config_opt *opt, enum config_context context)
{
    MMAL_PARAM_EXPOSUREMODE_T mode;
    const char *str = getenv(opt->env_key);
    if(strcmp(str, "off") == 0) mode = MMAL_PARAM_EXPOSUREMODE_OFF;
    else if(strcmp(str, "auto") == 0) mode = MMAL_PARAM_EXPOSUREMODE_AUTO;
    else if(strcmp(str, "night") == 0) mode = MMAL_PARAM_EXPOSUREMODE_NIGHT;
    else if(strcmp(str, "nightpreview") == 0) mode = MMAL_PARAM_EXPOSUREMODE_NIGHTPREVIEW;
    else if(strcmp(str, "backlight") == 0) mode = MMAL_PARAM_EXPOSUREMODE_BACKLIGHT;
    else if(strcmp(str, "spotlight") == 0) mode = MMAL_PARAM_EXPOSUREMODE_SPOTLIGHT;
    else if(strcmp(str, "sports") == 0) mode = MMAL_PARAM_EXPOSUREMODE_SPORTS;
    else if(strcmp(str, "snow") == 0) mode = MMAL_PARAM_EXPOSUREMODE_SNOW;
    else if(strcmp(str, "beach") == 0) mode = MMAL_PARAM_EXPOSUREMODE_BEACH;
    else if(strcmp(str, "verylong") == 0) mode = MMAL_PARAM_EXPOSUREMODE_VERYLONG;
    else if(strcmp(str, "fixedfps") == 0) mode = MMAL_PARAM_EXPOSUREMODE_FIXEDFPS;
    else if(strcmp(str, "antishake") == 0) mode = MMAL_PARAM_EXPOSUREMODE_ANTISHAKE;
    else if(strcmp(str, "fireworks") == 0) mode = MMAL_PARAM_EXPOSUREMODE_FIREWORKS;
    else {
        if (context == config_context_server_start)
            errx(EXIT_FAILURE, "Invalid %s", opt->long_option);
        else
            return;
    }

    MMAL_PARAMETER_EXPOSUREMODE_T param = {{MMAL_PARAMETER_EXPOSURE_MODE,sizeof(param)}, mode};
    if (mmal_port_parameter_set(state.camera->control, &param.hdr) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not set %s", opt->long_option);
}
static void awb_apply(const struct raspi_config_opt *opt, enum config_context context)
{
    MMAL_PARAM_AWBMODE_T awb_mode;
    const char *str = getenv(opt->env_key);
    if(strcmp(str, "off") == 0) awb_mode = MMAL_PARAM_AWBMODE_OFF;
    else if(strcmp(str, "auto") == 0) awb_mode = MMAL_PARAM_AWBMODE_AUTO;
    else if(strcmp(str, "sun") == 0) awb_mode = MMAL_PARAM_AWBMODE_SUNLIGHT;
    else if(strcmp(str, "cloudy") == 0) awb_mode = MMAL_PARAM_AWBMODE_CLOUDY;
    else if(strcmp(str, "shade") == 0) awb_mode = MMAL_PARAM_AWBMODE_SHADE;
    else if(strcmp(str, "tungsten") == 0) awb_mode = MMAL_PARAM_AWBMODE_TUNGSTEN;
    else if(strcmp(str, "fluorescent") == 0) awb_mode = MMAL_PARAM_AWBMODE_FLUORESCENT;
    else if(strcmp(str, "incandescent") == 0) awb_mode = MMAL_PARAM_AWBMODE_INCANDESCENT;
    else if(strcmp(str, "flash") == 0) awb_mode = MMAL_PARAM_AWBMODE_FLASH;
    else if(strcmp(str, "horizon") == 0) awb_mode = MMAL_PARAM_AWBMODE_HORIZON;
    else {
        if (context == config_context_server_start)
            errx(EXIT_FAILURE, "Invalid %s", opt->long_option);
        else
            return;
    }
    MMAL_PARAMETER_AWBMODE_T param = {{MMAL_PARAMETER_AWB_MODE,sizeof(param)}, awb_mode};
    if (mmal_port_parameter_set(state.camera->control, &param.hdr) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not set %s", opt->long_option);
}
static void imxfx_apply(const struct raspi_config_opt *opt, enum config_context context)
{
    MMAL_PARAM_IMAGEFX_T imageFX;
    const char *str = getenv(opt->env_key);
    if(strcmp(str, "none") == 0) imageFX = MMAL_PARAM_IMAGEFX_NONE;
    else if(strcmp(str, "negative") == 0) imageFX = MMAL_PARAM_IMAGEFX_NEGATIVE;
    else if(strcmp(str, "solarise") == 0) imageFX = MMAL_PARAM_IMAGEFX_SOLARIZE;
    else if(strcmp(str, "solarize") == 0) imageFX = MMAL_PARAM_IMAGEFX_SOLARIZE;
    else if(strcmp(str, "sketch") == 0) imageFX = MMAL_PARAM_IMAGEFX_SKETCH;
    else if(strcmp(str, "denoise") == 0) imageFX = MMAL_PARAM_IMAGEFX_DENOISE;
    else if(strcmp(str, "emboss") == 0) imageFX = MMAL_PARAM_IMAGEFX_EMBOSS;
    else if(strcmp(str, "oilpaint") == 0) imageFX = MMAL_PARAM_IMAGEFX_OILPAINT;
    else if(strcmp(str, "hatch") == 0) imageFX = MMAL_PARAM_IMAGEFX_HATCH;
    else if(strcmp(str, "gpen") == 0) imageFX = MMAL_PARAM_IMAGEFX_GPEN;
    else if(strcmp(str, "pastel") == 0) imageFX = MMAL_PARAM_IMAGEFX_PASTEL;
    else if(strcmp(str, "watercolour") == 0) imageFX = MMAL_PARAM_IMAGEFX_WATERCOLOUR;
    else if(strcmp(str, "watercolor") == 0) imageFX = MMAL_PARAM_IMAGEFX_WATERCOLOUR;
    else if(strcmp(str, "film") == 0) imageFX = MMAL_PARAM_IMAGEFX_FILM;
    else if(strcmp(str, "blur") == 0) imageFX = MMAL_PARAM_IMAGEFX_BLUR;
    else if(strcmp(str, "saturation") == 0) imageFX = MMAL_PARAM_IMAGEFX_SATURATION;
    else if(strcmp(str, "colourswap") == 0) imageFX = MMAL_PARAM_IMAGEFX_COLOURSWAP;
    else if(strcmp(str, "colorswap") == 0) imageFX = MMAL_PARAM_IMAGEFX_COLOURSWAP;
    else if(strcmp(str, "washedout") == 0) imageFX = MMAL_PARAM_IMAGEFX_WASHEDOUT;
    else if(strcmp(str, "posterise") == 0) imageFX = MMAL_PARAM_IMAGEFX_POSTERISE;
    else if(strcmp(str, "posterize") == 0) imageFX = MMAL_PARAM_IMAGEFX_POSTERISE;
    else if(strcmp(str, "colourpoint") == 0) imageFX = MMAL_PARAM_IMAGEFX_COLOURPOINT;
    else if(strcmp(str, "colorpoint") == 0) imageFX = MMAL_PARAM_IMAGEFX_COLOURPOINT;
    else if(strcmp(str, "colourbalance") == 0) imageFX = MMAL_PARAM_IMAGEFX_COLOURBALANCE;
    else if(strcmp(str, "colorbalance") == 0) imageFX = MMAL_PARAM_IMAGEFX_COLOURBALANCE;
    else if(strcmp(str, "cartoon") == 0) imageFX = MMAL_PARAM_IMAGEFX_CARTOON;
    else {
        if (context == config_context_server_start)
            errx(EXIT_FAILURE, "Invalid %s", opt->long_option);
        else
            return;
    }
    MMAL_PARAMETER_IMAGEFX_T param = {{MMAL_PARAMETER_IMAGE_EFFECT,sizeof(param)}, imageFX};
    if (mmal_port_parameter_set(state.camera->control, &param.hdr) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not set %s", opt->long_option);
}
static void colfx_apply(const struct raspi_config_opt *opt, enum config_context context)
{
    // Color effect is specified as u:v. Anything else means off.
    MMAL_PARAMETER_COLOURFX_T param = {{MMAL_PARAMETER_COLOUR_EFFECT,sizeof(param)}, 0, 0, 0};
    const char *str = getenv(opt->env_key);
    if (sscanf(str, "%d:%d", &param.u, &param.v) == 2 &&
            param.u < 256 &&
            param.v < 256)
        param.enable = 1;
    else
        param.enable = 0;
    if (mmal_port_parameter_set(state.camera->control, &param.hdr) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not set %s", opt->long_option);
}
static void metering_apply(const struct raspi_config_opt *opt, enum config_context context)
{
    MMAL_PARAM_EXPOSUREMETERINGMODE_T m_mode;
    const char *str = getenv(opt->env_key);
    if(strcmp(str, "average") == 0) m_mode = MMAL_PARAM_EXPOSUREMETERINGMODE_AVERAGE;
    else if(strcmp(str, "spot") == 0) m_mode = MMAL_PARAM_EXPOSUREMETERINGMODE_SPOT;
    else if(strcmp(str, "backlit") == 0) m_mode = MMAL_PARAM_EXPOSUREMETERINGMODE_BACKLIT;
    else if(strcmp(str, "matrix") == 0) m_mode = MMAL_PARAM_EXPOSUREMETERINGMODE_MATRIX;
    else {
        if (context == config_context_server_start)
            errx(EXIT_FAILURE, "Invalid %s", opt->long_option);
        else
            return;
    }
    MMAL_PARAMETER_EXPOSUREMETERINGMODE_T param = {{MMAL_PARAMETER_EXP_METERING_MODE,sizeof(param)}, m_mode};
    if (mmal_port_parameter_set(state.camera->control, &param.hdr) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not set %s", opt->long_option);
}
static void rotation_apply(const struct raspi_config_opt *opt, enum config_context context)
{
    UNUSED(context);
    int value = strtol(getenv(opt->env_key), NULL, 0);
    if (mmal_port_parameter_set_int32(state.camera->output[0], MMAL_PARAMETER_ROTATION, value) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not set %s", opt->long_option);
}
static void flip_apply(const struct raspi_config_opt *opt, enum config_context context)
{
    UNUSED(context);

    MMAL_PARAMETER_MIRROR_T mirror = {{MMAL_PARAMETER_MIRROR, sizeof(MMAL_PARAMETER_MIRROR_T)}, MMAL_PARAM_MIRROR_NONE};
    if (strcmp(getenv(RASPIJPGS_HFLIP), "on") == 0)
        mirror.value = MMAL_PARAM_MIRROR_HORIZONTAL;
    if (strcmp(getenv(RASPIJPGS_VFLIP), "on") == 0)
        mirror.value = (mirror.value == MMAL_PARAM_MIRROR_HORIZONTAL ? MMAL_PARAM_MIRROR_BOTH : MMAL_PARAM_MIRROR_VERTICAL);

    if (mmal_port_parameter_set(state.camera->output[0], &mirror.hdr) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not set %s", opt->long_option);
}
static void sensor_mode_apply(const struct raspi_config_opt *opt, enum config_context context)
{
    // TODO
    UNUSED(opt);
    UNUSED(context);
}
static void roi_apply(const struct raspi_config_opt *opt, enum config_context context)
{
    // TODO
    UNUSED(opt);
    UNUSED(context);
}
static void shutter_apply(const struct raspi_config_opt *opt, enum config_context context)
{
    UNUSED(context);
    int value = strtoul(getenv(opt->env_key), NULL, 0);
    if (mmal_port_parameter_set_uint32(state.camera->control, MMAL_PARAMETER_SHUTTER_SPEED, value) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not set %s", opt->long_option);
}
static void quality_apply(const struct raspi_config_opt *opt, enum config_context context)
{
    UNUSED(context);
    int value = strtoul(getenv(opt->env_key), NULL, 0);
    value = constrain(0, value, 100);
    if (mmal_port_parameter_set_uint32(state.jpegencoder->output[0], MMAL_PARAMETER_JPEG_Q_FACTOR, value) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not set %s to %d", opt->long_option, value);
}
static void restart_interval_apply(const struct raspi_config_opt *opt, enum config_context context)
{
    // TODO
    UNUSED(opt);
    UNUSED(context);
}
static void fps_apply(const struct raspi_config_opt *opt, enum config_context context)
{
    // TODO
    UNUSED(opt);
    UNUSED(context);
}

static struct raspi_config_opt opts[] =
{
    // long_option  short   env_key                  help                                                    default
    {"width",       "w",    RASPIJPGS_WIDTH,        "Set image width <size>",                               "320",      default_set, width_apply},
    {"height",      "h",    RASPIJPGS_HEIGHT,       "Set image height <size> (0 = calculate from width",    "0",        default_set, height_apply},
    {"annotation",  "a",    RASPIJPGS_ANNOTATION,   "Annotate the video frames with this text",             "",         default_set, annotation_apply},
    {"anno_background", "ab", RASPIJPGS_ANNO_BACKGROUND, "Turn on a black background behind the annotation", "off",     default_set, anno_background_apply},
    {"sharpness",   "sh",   RASPIJPGS_SHARPNESS,    "Set image sharpness (-100 to 100)",                    "0",        default_set, sharpness_apply},
    {"contrast",    "co",   RASPIJPGS_CONTRAST,     "Set image contrast (-100 to 100)",                     "0",        default_set, contrast_apply},
    {"brightness",  "br",   RASPIJPGS_BRIGHTNESS,   "Set image brightness (0 to 100)",                      "50",       default_set, brightness_apply},
    {"saturation",  "sa",   RASPIJPGS_SATURATION,   "Set image saturation (-100 to 100)",                   "0",        default_set, saturation_apply},
    {"ISO",         "ISO",  RASPIJPGS_ISO,          "Set capture ISO (100 to 800)",                         "0",        default_set, ISO_apply},
    {"vstab",       "vs",   RASPIJPGS_VSTAB,        "Turn on video stabilisation",                          "off",      default_set, vstab_apply},
    {"ev",          "ev",   RASPIJPGS_EV,           "Set EV compensation (-10 to 10)",                      "0",        default_set, ev_apply},
    {"exposure",    "ex",   RASPIJPGS_EXPOSURE,     "Set exposure mode",                                    "auto",     default_set, exposure_apply},
    {"fps",         0,      RASPIJPGS_FPS,          "Limit the frame rate (0 = auto)",                      "0",        default_set, fps_apply},
    {"awb",         "awb",  RASPIJPGS_AWB,          "Set Automatic White Balance (AWB) mode",               "auto",     default_set, awb_apply},
    {"imxfx",       "ifx",  RASPIJPGS_IMXFX,        "Set image effect",                                     "none",     default_set, imxfx_apply},
    {"colfx",       "cfx",  RASPIJPGS_COLFX,        "Set colour effect <U:V>",                              "",         default_set, colfx_apply},
    {"mode",        "md",   RASPIJPGS_SENSOR_MODE,  "Set sensor mode (0 to 7)",                             "0",        default_set, sensor_mode_apply},
    {"metering",    "mm",   RASPIJPGS_METERING,     "Set metering mode",                                    "average",  default_set, metering_apply},
    {"rotation",    "rot",  RASPIJPGS_ROTATION,     "Set image rotation (0-359)",                           "0",        default_set, rotation_apply},
    {"hflip",       "hf",   RASPIJPGS_HFLIP,        "Set horizontal flip",                                  "off",      default_set, flip_apply},
    {"vflip",       "vf",   RASPIJPGS_VFLIP,        "Set vertical flip",                                    "off",      default_set, flip_apply},
    {"roi",         "roi",  RASPIJPGS_ROI,          "Set region of interest (x,y,w,d as normalised coordinates [0.0-1.0])", "0:0:1:1", default_set, roi_apply},
    {"shutter",     "ss",   RASPIJPGS_SHUTTER,      "Set shutter speed",                                    "0",        default_set, shutter_apply},
    {"quality",     "q",    RASPIJPGS_QUALITY,      "Set the JPEG quality (0-100)",                         "15",       default_set, quality_apply},
    {"restart_interval", "rs", RASPIJPGS_RESTART_INTERVAL, "Set the JPEG restart interval (default of 0 for none)", "0", default_set, restart_interval_apply},

    // options that can't be overridden using environment variables
    {"config",      "c",    0,                       "Specify a config file to read for options",            0,          config_set, 0},
    {"help",        "h",    0,                       "Print this help message",                              0,          help, 0},
    {0,             0,      0,                       0,                                                      0,          0,           0}
};

static void help(const struct raspi_config_opt *opt, const char *value, enum config_context context)
{
    UNUSED(opt); UNUSED(value);

    // Don't provide help if this is a request from a connected client
    // since the user won't see it.
    if (context == config_context_client_request)
        return;

    fprintf(stderr, "raspijpgs [options]\n");

    const struct raspi_config_opt *o;
    for (o = opts; o->long_option; o++) {
        if (o->short_option)
            fprintf(stderr, "  --%-15s (-%s)\t %s\n", o->long_option, o->short_option, o->help);
        else
            fprintf(stderr, "  --%-20s\t %s\n", o->long_option, o->help);
    }

    fprintf(stderr,
            "\n"
            "Exposure (--exposure) options: auto, night, nightpreview, backlight,\n"
            "    spotlight, sports, snow, beach, verylong, fixedfps, antishake,\n"
            "    fireworks\n"
            "White balance (--awb) options: auto, sun, cloudy, shade, tungsten,\n"
            "    fluorescent, flash, horizon\n"
            "Image effect (--imxfx) options: none, negative, solarize, sketch,\n"
            "    denoise, emboss, oilpaint, hatch, gpen, pastel, watercolor, film,\n"
            "    blur, saturation, colorswap, washedout, posterize, colorpoint,\n"
            "    colorbalance, cartoon\n"
            "Metering (--metering) options: average, spot, backlit, matrix\n"
            "Sensor mode (--mode) options:\n"
            "       0   automatic selection\n"
            "       1   1920x1080 (16:9) 1-30 fps\n"
            "       2   2592x1944 (4:3)  1-15 fps\n"
            "       3   2592x1944 (4:3)  0.1666-1 fps\n"
            "       4   1296x972  (4:3)  1-42 fps, 2x2 binning\n"
            "       5   1296x730  (16:9) 1-49 fps, 2x2 binning\n"
            "       6   640x480   (4:3)  42.1-60 fps, 2x2 binning plus skip\n"
            "       7   640x480   (4:3)  60.1-90 fps, 2x2 binning plus skip\n"
            );

    // It make sense to exit in all non-client request contexts
    exit(EXIT_FAILURE);
}

static int is_long_option(const char *str)
{
    return strlen(str) >= 3 && str[0] == '-' && str[1] == '-';
}
static int is_short_option(const char *str)
{
    return strlen(str) >= 2 && str[0] == '-' && str[1] != '-';
}

static void fillin_defaults()
{
    const struct raspi_config_opt *opt;
    for (opt = opts; opt->long_option; opt++) {
        if (opt->env_key && opt->default_value) {
            // The replace option is set to 0, so that anything set in the environment
            // is an override.
            if (setenv(opt->env_key, opt->default_value, 0) < 0)
                err(EXIT_FAILURE, "Error setting %s to %s", opt->env_key, opt->default_value);
        }
    }
}

static void apply_parameters(enum config_context context)
{
    const struct raspi_config_opt *opt;
    for (opt = opts; opt->long_option; opt++) {
        if (opt->apply)
            opt->apply(opt, context);
    }
}

static void parse_args(int argc, char *argv[])
{
    int i;
    for (i = 1; i < argc; i++) {
        const struct raspi_config_opt *opt = 0;
        char *value;
        if (is_long_option(argv[i])) {
            char *key = argv[i] + 2; // skip over "--"
            value = strchr(argv[i], '=');
            if (value)
                *value = '\0'; // zap the '=' so that key is trimmed
            for (opt = opts; opt->long_option; opt++)
                if (strcmp(opt->long_option, key) == 0)
                    break;
            if (!opt->long_option) {
                warnx("Unknown option '%s'", key);
                help(0, 0, 0);
            }

            if (!value && i < argc - 1 && !is_long_option(argv[i + 1]) && !is_short_option(argv[i + 1]))
                value = argv[++i];
            if (!value)
                value = "on"; // if no value, then this is a boolean argument, so set to on
        } else if (is_short_option(argv[i])) {
            char *key = argv[i] + 1; // skip over the "-"
            for (opt = opts; opt->long_option; opt++)
                if (opt->short_option && strcmp(opt->short_option, key) == 0)
                    break;
            if (!opt->long_option) {
                warnx("Unknown option '%s'", key);
                help(0, 0, 0);
            }

            if (i < argc - 1)
                value = argv[++i];
            else
                value = "on"; // if no value, then this is a boolean argument, so set to on
        } else {
            warnx("Unexpected parameter '%s'", argv[i]);
            help(0, 0, 0);
        }

        if (opt)
            opt->set(opt, value, config_context_parse_cmdline);
    }
}

static void trim_whitespace(char *s)
{
    char *left = s;
    while (*left != 0 && isspace(*left))
        left++;
    char *right = s + strlen(s) - 1;
    while (right >= left && isspace(*right))
        right--;

    int len = right - left + 1;
    if (len)
        memmove(s, left, len);
    s[len] = 0;
}

static void parse_config_line(const char *line, enum config_context context)
{
    char *str = strdup(line);
    // Trim everything after a comment
    char *comment = strchr(str, '#');
    if (comment)
        *comment = '\0';

    // Trim whitespace off the beginning and end
    trim_whitespace(str);

    if (*str == '\0') {
        free(str);
        return;
    }

    char *key = str;
    char *value = strchr(str, '=');
    if (value) {
        *value = '\0';
        value++;
        trim_whitespace(value);
        trim_whitespace(key);
    } else
        value = "on";

    const struct raspi_config_opt *opt;
    for (opt = opts; opt->long_option; opt++)
        if (strcmp(opt->long_option, key) == 0)
            break;
    if (!opt->long_option) {
        // Error out if we're parsing a file; otherwise ignore the bad option
        if (context == config_context_file)
            errx(EXIT_FAILURE, "Unknown option '%s' in file '%s'", key, state.config_filename);
        else {
            free(str);
            return;
        }
    }

    switch (context) {
    case config_context_file:
        opt->set(opt, value, context);
        break;

    case config_context_client_request:
        opt->set(opt, value, context);
        if (opt->apply)
            opt->apply(opt, context);
        break;

    default:
        // Ignore
        break;
    }
    free(str);
}

static void load_config_file()
{
    if (!state.config_filename)
        return;

    FILE *fp = fopen(state.config_filename, "r");
    if (!fp)
        err(EXIT_FAILURE, "Cannot open '%s'", state.config_filename);

    char line[128];
    while (fgets(line, sizeof(line), fp))
        parse_config_line(line, config_context_file);

    fclose(fp);
}

static void camera_control_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
    // This is called from another thread. Don't access any data here.
    UNUSED(port);

    if (buffer->cmd == MMAL_EVENT_ERROR)
       errx(EXIT_FAILURE, "No data received from sensor. Check all connections, including the Sunny one on the camera board");
    else if(buffer->cmd != MMAL_EVENT_PARAMETER_CHANGED)
        errx(EXIT_FAILURE, "Camera sent invalid data: 0x%08x", buffer->cmd);

    mmal_buffer_header_release(buffer);
}

static void output_jpeg(const char *buf, int len)
{
    struct iovec iovs[2];
    uint32_t len32 = htonl(len);
    iovs[0].iov_base = &len32;
    iovs[0].iov_len = sizeof(int32_t);
    iovs[1].iov_base = (char *) buf; // silence warning
    iovs[1].iov_len = len;
    ssize_t count = writev(STDOUT_FILENO, iovs, 2);
    if (count < 0)
        err(EXIT_FAILURE, "Error writing to stdout");
    else if (count != (ssize_t) (iovs[0].iov_len + iovs[1].iov_len))
        warnx("Unexpected truncation of JPEG when writing to stdout");
}

static void distribute_jpeg(const char *buf, size_t len)
{
    // Handle it ourselves
    output_jpeg(buf, len);
}

static void recycle_jpegencoder_buffer(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
    mmal_buffer_header_release(buffer);

    if (port->is_enabled) {
        MMAL_BUFFER_HEADER_T *new_buffer;

        if (!(new_buffer = mmal_queue_get(state.pool_jpegencoder->queue)) ||
             mmal_port_send_buffer(port, new_buffer) != MMAL_SUCCESS)
            errx(EXIT_FAILURE, "Could not send buffers to port");
    }
}

static void jpegencoder_buffer_callback_impl()
{
    void *msg[2];
    if (read(state.mmal_callback_pipe[0], msg, sizeof(msg)) != sizeof(msg))
        err(EXIT_FAILURE, "read from internal pipe broke");

    MMAL_PORT_T *port = (MMAL_PORT_T *) msg[0];
    MMAL_BUFFER_HEADER_T *buffer = (MMAL_BUFFER_HEADER_T *) msg[1];

    mmal_buffer_header_mem_lock(buffer);

    if (state.socket_buffer_ix == 0 &&
            (buffer->flags & MMAL_BUFFER_HEADER_FLAG_FRAME_END) &&
            buffer->length <= MAX_DATA_BUFFER_SIZE) {
        // Easy case: JPEG all in one buffer
        distribute_jpeg((const char *) buffer->data, buffer->length);
    } else {
        // Hard case: assemble JPEG
        if (state.socket_buffer_ix + buffer->length > MAX_DATA_BUFFER_SIZE) {
            if (buffer->flags & MMAL_BUFFER_HEADER_FLAG_FRAME_END) {
                state.socket_buffer_ix = 0;
            } else if (state.socket_buffer_ix != MAX_DATA_BUFFER_SIZE) {
                // Warn when frame crosses threshold
                warnx("Frame too large (%d bytes). Dropping. Adjust MAX_DATA_BUFFER_SIZE.", state.socket_buffer_ix + buffer->length);
                state.socket_buffer_ix = MAX_DATA_BUFFER_SIZE;
            }
        } else {
            memcpy(&state.socket_buffer[state.socket_buffer_ix], buffer->data, buffer->length);
            state.socket_buffer_ix += buffer->length;
            if (buffer->flags & MMAL_BUFFER_HEADER_FLAG_FRAME_END) {
                distribute_jpeg(state.socket_buffer, state.socket_buffer_ix);
                state.socket_buffer_ix = 0;
            }
        }
    }

    mmal_buffer_header_mem_unlock(buffer);

    //cam_set_annotation();

    recycle_jpegencoder_buffer(port, buffer);
}

static void jpegencoder_buffer_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
    // If the buffer contains something, notify our main thread to process it.
    // If not, recycle it.
    if (buffer->length) {
        void *msg[2];
        msg[0] = port;
        msg[1] = buffer;
        if (write(state.mmal_callback_pipe[1], msg, sizeof(msg)) != sizeof(msg))
            err(EXIT_FAILURE, "write to internal pipe broke");
    } else {
        recycle_jpegencoder_buffer(port, buffer);
    }
}

static void find_sensor_dimensions(unsigned int camera_ix, int *imager_width, int *imager_height)
{
    MMAL_COMPONENT_T *camera_info;

    // Default to OV5647 full resolution
    *imager_width = 2592;
    *imager_height = 1944;

    // Try to get the camera name and maximum supported resolution
    MMAL_STATUS_T status = mmal_component_create(MMAL_COMPONENT_DEFAULT_CAMERA_INFO, &camera_info);
    if (status == MMAL_SUCCESS) {
        MMAL_PARAMETER_CAMERA_INFO_T param;
        param.hdr.id = MMAL_PARAMETER_CAMERA_INFO;
        param.hdr.size = sizeof(param)-4;  // Deliberately undersize to check firmware version
        status = mmal_port_parameter_get(camera_info->control, &param.hdr);

        if (status != MMAL_SUCCESS) {
            // Running on newer firmware
            param.hdr.size = sizeof(param);
            status = mmal_port_parameter_get(camera_info->control, &param.hdr);
            if (status == MMAL_SUCCESS && param.num_cameras > camera_ix) {
                // Take the parameters from the first camera listed.
                *imager_width = param.cameras[camera_ix].max_width;
                *imager_height = param.cameras[camera_ix].max_height;
            } else
                warnx("Cannot read camera info, keeping the defaults for OV5647");
        } else {
            // Older firmware
            // Nothing to do here, keep the defaults for OV5647
        }

        mmal_component_destroy(camera_info);
    } else {
        warnx("Failed to create camera_info component");
    }
}

void start_all()
{
    // Find out which Raspberry Camera is attached for the defaults
    int imager_width;
    int imager_height;
    find_sensor_dimensions(0, &imager_width, &imager_height);

    //
    // create camera
    //
    if (mmal_component_create(MMAL_COMPONENT_DEFAULT_CAMERA, &state.camera) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not create camera");
    if (mmal_port_enable(state.camera->control, camera_control_callback) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not enable camera control port");

    int fps100 = lrint(100.0 * strtod(getenv(RASPIJPGS_FPS), 0));
    int width = strtol(getenv(RASPIJPGS_WIDTH), 0, 0);
    if (width <= 0)
        width = 320;
    else if (width > imager_width)
        width = imager_width;
    width = width & ~0xf; // Force to multiple of 16 for JPEG encoder
    int height = strtol(getenv(RASPIJPGS_HEIGHT), 0, 0);
    if (height <= 0)
        height = imager_height * width / imager_width; // Default to the camera's aspect ratio
    else if (height > imager_height)
        height = imager_height;
    height = height & ~0xf;

    // TODO: The fact that this seems to work implies that there's a scaler
    //       in the camera block and we don't need a resizer??
    int video_width = width;
    int video_height = height;

    MMAL_PARAMETER_CAMERA_CONFIG_T cam_config = {
        {MMAL_PARAMETER_CAMERA_CONFIG, sizeof(cam_config)},
        .max_stills_w = 0,
        .max_stills_h = 0,
        .stills_yuv422 = 0,
        .one_shot_stills = 0,
        .max_preview_video_w = imager_width,
        .max_preview_video_h = imager_height,
        .num_preview_video_frames = 3,
        .stills_capture_circular_buffer_height = 0,
        .fast_preview_resume = 0,
        .use_stc_timestamp = MMAL_PARAM_TIMESTAMP_MODE_RESET_STC
    };
    if (mmal_port_parameter_set(state.camera->control, &cam_config.hdr) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Error configuring camera");

    MMAL_ES_FORMAT_T *format = state.camera->output[0]->format;
    format->es->video.width = video_width;
    format->es->video.height = video_height;
    format->es->video.crop.x = 0;
    format->es->video.crop.y = 0;
    format->es->video.crop.width = video_width;
    format->es->video.crop.height = video_height;
    format->es->video.frame_rate.num = fps100;
    format->es->video.frame_rate.den = 100;
    if (mmal_port_format_commit(state.camera->output[0]) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not set preview format");

    if (mmal_component_enable(state.camera) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not enable camera");

    //
    // create jpeg-encoder
    //
    MMAL_STATUS_T status = mmal_component_create(MMAL_COMPONENT_DEFAULT_IMAGE_ENCODER, &state.jpegencoder);
    if (status != MMAL_SUCCESS && status != MMAL_ENOSYS)
        errx(EXIT_FAILURE, "Could not create image encoder");

    state.jpegencoder->output[0]->format->encoding = MMAL_ENCODING_JPEG;
    state.jpegencoder->output[0]->buffer_size = state.jpegencoder->output[0]->buffer_size_recommended;
    if (state.jpegencoder->output[0]->buffer_size < state.jpegencoder->output[0]->buffer_size_min)
        state.jpegencoder->output[0]->buffer_size = state.jpegencoder->output[0]->buffer_size_min;
    state.jpegencoder->output[0]->buffer_num = state.jpegencoder->output[0]->buffer_num_recommended;
    if(state.jpegencoder->output[0]->buffer_num < state.jpegencoder->output[0]->buffer_num_min)
        state.jpegencoder->output[0]->buffer_num = state.jpegencoder->output[0]->buffer_num_min;
    if (mmal_port_format_commit(state.jpegencoder->output[0]) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not set image format");

    int quality = strtol(getenv(RASPIJPGS_QUALITY), 0, 0);
    if (mmal_port_parameter_set_uint32(state.jpegencoder->output[0], MMAL_PARAMETER_JPEG_Q_FACTOR, quality) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not set jpeg quality to %d", quality);

    // Set the JPEG restart interval
    int restart_interval = strtol(getenv(RASPIJPGS_RESTART_INTERVAL), 0, 0);
    if (mmal_port_parameter_set_uint32(state.jpegencoder->output[0], MMAL_PARAMETER_JPEG_RESTART_INTERVAL, restart_interval) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Unable to set JPEG restart interval");

    if (mmal_port_parameter_set_boolean(state.jpegencoder->output[0], MMAL_PARAMETER_EXIF_DISABLE, 1) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not turn off EXIF");

    if (mmal_component_enable(state.jpegencoder) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not enable image encoder");
    state.pool_jpegencoder = mmal_port_pool_create(state.jpegencoder->output[0], state.jpegencoder->output[0]->buffer_num, state.jpegencoder->output[0]->buffer_size);
    if (!state.pool_jpegencoder)
        errx(EXIT_FAILURE, "Could not create image buffer pool");

    //
    // create image-resizer
    //
    status = mmal_component_create("vc.ril.resize", &state.resizer);
    if (status != MMAL_SUCCESS && status != MMAL_ENOSYS)
        errx(EXIT_FAILURE, "Could not create image resizer");

    format = state.resizer->output[0]->format;
    format->es->video.width = width;
    format->es->video.height = height;
    format->es->video.crop.x = 0;
    format->es->video.crop.y = 0;
    format->es->video.crop.width = width;
    format->es->video.crop.height = height;
    format->es->video.frame_rate.num = fps100;
    format->es->video.frame_rate.den = 1;
    if (mmal_port_format_commit(state.resizer->output[0]) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not set image resizer output");

    if (mmal_component_enable(state.resizer) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not enable image resizer");

    //
    // connect
    //
    if (mmal_connection_create(&state.con_cam_res, state.camera->output[0], state.resizer->input[0], MMAL_CONNECTION_FLAG_TUNNELLING | MMAL_CONNECTION_FLAG_ALLOCATION_ON_INPUT) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not create connection camera -> resizer");
    if (mmal_connection_enable(state.con_cam_res) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not enable connection camera -> resizer");

    if (mmal_connection_create(&state.con_res_jpeg, state.resizer->output[0], state.jpegencoder->input[0], MMAL_CONNECTION_FLAG_TUNNELLING | MMAL_CONNECTION_FLAG_ALLOCATION_ON_INPUT) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not create connection resizer -> encoder");
    if (mmal_connection_enable(state.con_res_jpeg) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not enable connection resizer -> encoder");

    if (mmal_port_enable(state.jpegencoder->output[0], jpegencoder_buffer_callback) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not enable jpeg port");
    int max = mmal_queue_length(state.pool_jpegencoder->queue);
    int i;
    for (i = 0; i < max; i++) {
        MMAL_BUFFER_HEADER_T *jpegbuffer = mmal_queue_get(state.pool_jpegencoder->queue);
        if (!jpegbuffer)
            errx(EXIT_FAILURE, "Could not create jpeg buffer header");
        if (mmal_port_send_buffer(state.jpegencoder->output[0], jpegbuffer) != MMAL_SUCCESS)
            errx(EXIT_FAILURE, "Could not send buffers to jpeg port");
    }

}

void stop_all()
{
    mmal_port_disable(state.jpegencoder->output[0]);
    mmal_connection_destroy(state.con_cam_res);
    mmal_connection_destroy(state.con_res_jpeg);
    mmal_port_pool_destroy(state.jpegencoder->output[0], state.pool_jpegencoder);
    mmal_component_disable(state.jpegencoder);
    mmal_component_disable(state.camera);
    mmal_component_destroy(state.jpegencoder);
    mmal_component_destroy(state.camera);
}

static void parse_config_lines(char *lines)
{
    char *line = lines;
    char *line_end;
    do {
        line_end = strchr(line, '\n');
        if (line_end)
            *line_end = '\0';
        parse_config_line(line, config_context_client_request);
        line = line_end + 1;
    } while (line_end);
}

static unsigned int from_uint32_be(const char *buffer)
{
    uint8_t *buf = (uint8_t*) buffer;
    return (buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | buf[3];
}

static void process_stdin_header_framing()
{
    // Each packet is length (4 bytes big endian), data
    int len = 0;
    while (state.stdin_buffer_ix > 4 &&
           (len = from_uint32_be(state.stdin_buffer)) &&
           state.stdin_buffer_ix >= 4 + len) {
        // Copy over the lines to process so that they can be
        // null terminated.
        char lines[len + 1];
        memcpy(lines, state.stdin_buffer + 4, len);
        lines[len] = '\0';

        parse_config_lines(lines);

        // Advance to the next packet
        state.stdin_buffer_ix -= 4 + len;
        memmove(state.stdin_buffer, state.stdin_buffer + 4 + len, state.stdin_buffer_ix);
    }

    // Check if we got a bogus length packet
    if (len >= MAX_DATA_BUFFER_SIZE - 4 - 1)
        errx(EXIT_FAILURE, "Invalid packet size. Out of sync?");
}

static int server_service_stdin()
{
    // Make sure that we have room to receive more data. If not,
    // a line is ridiculously long or a frame is messed up, so exit.
    if (state.stdin_buffer_ix >= MAX_REQUEST_BUFFER_SIZE - 1)
        err(EXIT_FAILURE, "Line too long on stdin");

    // Read in everything on stdin and see what gets processed
    int amount_read = read(STDIN_FILENO, &state.stdin_buffer[state.stdin_buffer_ix], MAX_REQUEST_BUFFER_SIZE - state.stdin_buffer_ix - 1);
    if (amount_read < 0)
        err(EXIT_FAILURE, "Error reading stdin");

    // Check if stdin was closed.
    if (amount_read == 0)
        return 0;

    state.stdin_buffer_ix += amount_read;

    // If we're in header framing mode, then everything sent and
    // received is prepended by a length. Otherwise it's just text
    // lines.
        process_stdin_header_framing();

    return amount_read;
}

static void server_service_mmal()
{
    jpegencoder_buffer_callback_impl();
}

static void server_loop()
{
    if (isatty(STDIN_FILENO))
        errx(EXIT_FAILURE, "stdin should be a program and not a tty");

    // Check if the user meant to run as a client and the server is dead
    if (state.sendlist)
        errx(EXIT_FAILURE, "Trying to send a message to a raspijpgs server, but one isn't running.");

    // Init hardware
    bcm_host_init();

    // Create the file descriptors for getting back to the main thread
    // from the MMAL callbacks.
    if (pipe(state.mmal_callback_pipe) < 0)
        err(EXIT_FAILURE, "pipe");

    start_all();
    apply_parameters(config_context_server_start);

    // Main loop - keep going until we don't want any more JPEGs.
    state.stdin_buffer = (char*) malloc(MAX_REQUEST_BUFFER_SIZE);
    struct pollfd fds[3];
    int fds_count = 2;
    fds[0].fd = state.mmal_callback_pipe[0];
    fds[0].events = POLLIN;
    fds[1].fd = STDIN_FILENO;
    fds[1].events = POLLIN;

    for (;;) {
        int ready = poll(fds, fds_count, 2000);
        if (ready < 0) {
            if (errno != EINTR)
                err(EXIT_FAILURE, "poll");
        } else if (ready == 0) {
            // Time out - something is wrong that we're not getting MMAL callbacks
            errx(EXIT_FAILURE, "MMAL unresponsive. Video stuck?");
        } else {
            if (fds[0].revents)
                server_service_mmal();
            if (fds[1].revents) {
                if (server_service_stdin() <= 0)
                    break;
            }
        }
    }

    stop_all();
    close(state.mmal_callback_pipe[0]);
    close(state.mmal_callback_pipe[1]);
    free(state.stdin_buffer);
}

int main(int argc, char* argv[])
{
    // Parse commandline and config file arguments
    parse_args(argc, argv);
    load_config_file();

    // If anything still isn't set, then fill-in with defaults
    fillin_defaults();

    // Allocate buffers
    state.socket_buffer = (char *) malloc(MAX_DATA_BUFFER_SIZE);
    if (!state.socket_buffer)
        err(EXIT_FAILURE, "malloc");

    server_loop();

    free(state.socket_buffer);

    exit(EXIT_SUCCESS);
}