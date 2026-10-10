#include "moonlight.hpp"
#include "diagstats.h"

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include "ppapi/lib/gl/gles2/gl2ext_ppapi.h"

#include "nacl_player/media_player.h"
#include "nacl_player/es_data_source.h"

#include <h264_stream.h>

#include <pthread.h>
#include <unistd.h>

#define INITIAL_DECODE_BUFFER_LEN 128 * 1024

#define HW_DECODER_INIT_ATTEMPTS 15
#define HW_DECODER_RETRY_DELAY_MS 200

static unsigned char* s_DecodeBuffer;
static unsigned int s_DecodeBufferLength;
static int s_LastTextureType;
static int s_LastTextureId;
static bool s_FirstFrameDisplayed;
static bool s_LoggedSps;
static uint64_t s_LastPaintFinishedTime;

namespace NaClPlayer = Samsung::NaClPlayer;

static std::unique_ptr<NaClPlayer::MediaPlayer> s_NaClPlayer;
static std::unique_ptr<NaClPlayer::ESDataSource> s_PlayerDataSource;
static std::unique_ptr<NaClPlayer::VideoElementaryStream> s_PlayerStream;
static bool s_PlayerDataSourceAttached;
static int s_PlayerFrameRate;
static unsigned int s_FirstPresentationTimeMs;
static unsigned int s_LastPresentationTimeMs;
static NaClPlayer::TimeTicks s_LastPacketPts;
static uint64_t s_NextAppendErrorLogMs;
static uint64_t s_PlayStartMs;
static uint64_t s_NextClockLogMs;
static uint32_t s_DiagFrames, s_DiagNonRefFrames, s_DiagMaxBytes;
static uint32_t s_DiagMinBytes = 0xFFFFFFFF;
volatile unsigned int s_PadTargetBytes = 0;

struct PlayerThreadWork {
    bool setup;
    int width;
    int height;
    int frameRate;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    bool complete;
    int result;
};

static void CleanupNaClPlayer() {
    if (s_PlayerDataSourceAttached) {
        int32_t result = s_NaClPlayer->Stop();
        MoonlightInstance::ClLogMessage("NaClPlayer: Stop=%d\n", result);
    }

    s_PlayerStream.reset();
    // AttachDataSource takes a reference with no nullable overload. Destroying
    // the source and player releases the attachment along with their resources.
    s_PlayerDataSource.reset();
    s_NaClPlayer.reset();
    s_PlayerDataSourceAttached = false;
}

static int SetupNaClPlayer(int width, int height, int frameRate) {
    if (frameRate <= 0) {
        MoonlightInstance::ClLogMessage("NaClPlayer: invalid frame rate=%d\n", frameRate);
        return -1;
    }

    s_PlayerFrameRate = frameRate;
    s_FirstPresentationTimeMs = 0;
    s_LastPresentationTimeMs = 0;
    s_LastPacketPts = -1.0;
    s_NextAppendErrorLogMs = 0;

    s_NaClPlayer.reset(new NaClPlayer::MediaPlayer(
        NaClPlayer::MediaPlayer::BindPlayerToDisplay,
        NaClPlayer::MediaPlayer::MediaPlayerModeLowLatency));
    bool valid = s_NaClPlayer->IsValid();
    MoonlightInstance::ClLogMessage("NaClPlayer: IsValid=%d\n", valid);
    if (!valid) {
        return -1;
    }

    s_PlayerDataSource.reset(new NaClPlayer::ESDataSource());
    s_PlayerStream.reset(new NaClPlayer::VideoElementaryStream());
    int32_t result = s_PlayerDataSource->AddStream<NaClPlayer::VideoElementaryStream>(
        *s_PlayerStream, nullptr);
    MoonlightInstance::ClLogMessage("NaClPlayer: AddStream=%d\n", result);
    if (result != NaClPlayer::ErrorCodes::Success) {
        return -1;
    }

    s_PlayerStream->SetVideoCodecType(NaClPlayer::VIDEOCODEC_TYPE_H264);
    s_PlayerStream->SetVideoCodecProfile(NaClPlayer::VIDEOCODEC_PROFILE_H264_HIGH);
    s_PlayerStream->SetVideoFrameFormat(NaClPlayer::VIDEOFRAME_FORMAT_I420);
    s_PlayerStream->SetVideoFrameSize(NaClPlayer::Size(width, height));
    s_PlayerStream->SetFrameRate(NaClPlayer::Rational(frameRate, 1));
    result = s_PlayerStream->InitializeDone();
    MoonlightInstance::ClLogMessage("NaClPlayer: InitializeDone=%d\n", result);
    if (result != NaClPlayer::ErrorCodes::Success) {
        return -1;
    }

    result = s_NaClPlayer->AttachDataSource(*s_PlayerDataSource);
    MoonlightInstance::ClLogMessage("NaClPlayer: AttachDataSource=%d\n", result);
    if (result != NaClPlayer::ErrorCodes::Success) {
        return -1;
    }
    s_PlayerDataSourceAttached = true;

    result = s_NaClPlayer->Play();
    MoonlightInstance::ClLogMessage("NaClPlayer: Play=%d\n", result);
    if (result != NaClPlayer::ErrorCodes::Success) {
        return -1;
    }
    s_PlayStartMs = LiGetMillis();
    s_NextClockLogMs = 0;

    g_Instance->PostMessage(pp::Var("nativePlayerActive"));
    g_Instance->PostMessage(pp::Var("displayVideo"));
    return 0;
}

static void PlayerThreadCallback(void* userData, int32_t result) {
    PlayerThreadWork* work = static_cast<PlayerThreadWork*>(userData);
    int workResult = -1;
    if (result == PP_OK) {
        if (work->setup) {
            workResult = SetupNaClPlayer(work->width, work->height, work->frameRate);
            if (workResult != 0) {
                CleanupNaClPlayer();
            }
        }
        else {
            CleanupNaClPlayer();
            workResult = 0;
        }
    }
    else {
        MoonlightInstance::ClLogMessage("NaClPlayer: player thread callback=%d\n", result);
    }

    pthread_mutex_lock(&work->mutex);
    work->result = workResult;
    work->complete = true;
    pthread_cond_signal(&work->condition);
    pthread_mutex_unlock(&work->mutex);
}

static int RunPlayerThreadWork(pp::SimpleThread* thread, bool setup,
                              int width = 0, int height = 0, int frameRate = 0) {
    if (thread == NULL) {
        MoonlightInstance::ClLogMessage("NaClPlayer: player thread unavailable\n");
        return -1;
    }

    PlayerThreadWork work = {
        setup, width, height, frameRate,
        PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, false, -1
    };
    pthread_mutex_lock(&work.mutex);
    int32_t result = thread->message_loop().PostWork(
        pp::CompletionCallback(&PlayerThreadCallback, &work));
    if (result == PP_OK) {
        while (!work.complete) {
            pthread_cond_wait(&work.condition, &work.mutex);
        }
    }
    else {
        MoonlightInstance::ClLogMessage("NaClPlayer: PostWork=%d\n", result);
    }
    pthread_mutex_unlock(&work.mutex);
    pthread_cond_destroy(&work.condition);
    pthread_mutex_destroy(&work.mutex);
    return work.result;
}

#define assertNoGLError() assert(!glGetError())

static const char k_VertexShader[] =
    "varying vec2 v_texCoord;            \n"
    "attribute vec4 a_position;          \n"
    "attribute vec2 a_texCoord;          \n"
    "uniform vec2 v_scale;               \n"
    "void main()                         \n"
    "{                                   \n"
    "    v_texCoord = v_scale * a_texCoord; \n"
    "    gl_Position = a_position;       \n"
    "}";

static const char k_FragmentShader2D[] =
    "precision mediump float;            \n"
    "varying vec2 v_texCoord;            \n"
    "uniform sampler2D s_texture;        \n"
    "void main()                         \n"
    "{"
    "    gl_FragColor = texture2D(s_texture, v_texCoord); \n"
    "}";
    
static const char k_FragmentShaderRectangle[] =
    "#extension GL_ARB_texture_rectangle : require\n"
    "precision mediump float;            \n"
    "varying vec2 v_texCoord;            \n"
    "uniform sampler2DRect s_texture;    \n"
    "void main()                         \n"
    "{"
    "    gl_FragColor = texture2DRect(s_texture, v_texCoord).rgba; \n"
    "}";
    
static const char k_FragmentShaderExternal[] =
      "#extension GL_OES_EGL_image_external : require\n"
      "precision mediump float;            \n"
      "varying vec2 v_texCoord;            \n"
      "uniform samplerExternalOES s_texture; \n"
      "void main()                         \n"
      "{"
      "    gl_FragColor = texture2D(s_texture, v_texCoord); \n"
      "}";

void MoonlightInstance::DidChangeFocus(bool got_focus) {
    // Request an IDR frame to dump the frame queue that may have
    // built up from the GL pipeline being stalled.
    if (got_focus) {
        LiRequestIdrFrame();
    }
}

void MoonlightInstance::DidChangeView(const pp::View& view) {
    m_PluginRect = view.GetRect();
}

bool MoonlightInstance::InitializeRenderingSurface(int width, int height) {
    if (!glInitializePPAPI(pp::Module::Get()->get_browser_interface())) {
        return false;
    }
    
    int32_t contextAttributes[] = {
        PP_GRAPHICS3DATTRIB_ALPHA_SIZE,     8,
        PP_GRAPHICS3DATTRIB_BLUE_SIZE,      8,
        PP_GRAPHICS3DATTRIB_GREEN_SIZE,     8,
        PP_GRAPHICS3DATTRIB_RED_SIZE,       8,
        PP_GRAPHICS3DATTRIB_DEPTH_SIZE,     0,
        PP_GRAPHICS3DATTRIB_STENCIL_SIZE,   0,
        PP_GRAPHICS3DATTRIB_SAMPLES,        0,
        PP_GRAPHICS3DATTRIB_SAMPLE_BUFFERS, 0,
        PP_GRAPHICS3DATTRIB_WIDTH,          width,
        PP_GRAPHICS3DATTRIB_HEIGHT,         height,
        PP_GRAPHICS3DATTRIB_NONE
    };
    g_Instance->m_Graphics3D = pp::Graphics3D(this, contextAttributes);
    if (g_Instance->m_Graphics3D.is_null()) {
        ClDisplayMessage("Unable to create OpenGL context");
        return false;
    }
    
    int32_t swapBehaviorAttribute[] = {
        PP_GRAPHICS3DATTRIB_SWAP_BEHAVIOR, PP_GRAPHICS3DATTRIB_BUFFER_DESTROYED,
        PP_GRAPHICS3DATTRIB_NONE
    };
    g_Instance->m_Graphics3D.SetAttribs(swapBehaviorAttribute);
    
    if (!BindGraphics(m_Graphics3D)) {
      ClDisplayMessage("Unable to bind OpenGL context");
      m_Graphics3D = pp::Graphics3D();
      glSetCurrentContextPPAPI(0);
      return false;
    }
    
    glSetCurrentContextPPAPI(m_Graphics3D.pp_resource());
    
    glDisable(GL_DITHER);
    
    glViewport(0, 0, width, height);
    
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    
    assertNoGLError();
    
    static const float k_Vertices[] = {
        -1, -1, -1, 1, 1, -1, 1, 1,  // Position coordinates.
        0,  1,  0,  0, 1, 1,  1, 0,  // Texture coordinates.
    };

    GLuint buffer;
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_ARRAY_BUFFER, buffer);

    glBufferData(GL_ARRAY_BUFFER,
                 sizeof(k_Vertices),
                 k_Vertices,
                 GL_STATIC_DRAW);
    assertNoGLError();
    
    g_Instance->m_Graphics3D.SwapBuffers(pp::BlockUntilComplete());
    return true;
}

int MoonlightInstance::VidDecSetup(int videoFormat, int width, int height, int redrawRate, void* context, int drFlags) {
    if (!g_Instance->m_UseNaClPlayer) {
        g_Instance->m_VideoDecoder = new pp::VideoDecoder(g_Instance);
    }
    
    s_DecodeBufferLength = INITIAL_DECODE_BUFFER_LEN;
    s_DecodeBuffer = (unsigned char *)malloc(s_DecodeBufferLength);
    s_LastTextureType = 0;
    s_LastTextureId = 0;
    s_FirstFrameDisplayed = false;
    s_LoggedSps = false;
    diag::Reset();

    if (g_Instance->m_UseNaClPlayer) {
        if (s_DecodeBuffer == NULL) {
            ClLogMessage("NaClPlayer: unable to allocate decode buffer\n");
            s_DecodeBufferLength = 0;
            return -1;
        }
        int result = RunPlayerThreadWork(g_Instance->m_PlayerThread, true, width, height, redrawRate);
        if (result != 0) {
            free(s_DecodeBuffer);
            s_DecodeBuffer = NULL;
            s_DecodeBufferLength = 0;
        }
        return result;
    }

    int32_t err;

    if (!(drFlags & DR_FLAG_FORCE_SW_DECODE)) {
        // Try to initialize hardware decoding only. The TV has a single hardware
        // decoder, and the previous stream's instance is released asynchronously,
        // so retry briefly rather than dropping to (unusably slow) CPU decoding.
        for (int attempt = 1; attempt <= HW_DECODER_INIT_ATTEMPTS; attempt++) {
            err = g_Instance->m_VideoDecoder->Initialize(
               g_Instance->m_Graphics3D,
               PP_VIDEOPROFILE_H264HIGH,
               PP_HARDWAREACCELERATION_ONLY,
               0,
               pp::BlockUntilComplete());
            if (err != PP_ERROR_NOTSUPPORTED) {
                break;
            }

            ClLogMessage("Hardware decoder busy (attempt %d of %d)\n", attempt, HW_DECODER_INIT_ATTEMPTS);
            usleep(HW_DECODER_RETRY_DELAY_MS * 1000);
            delete g_Instance->m_VideoDecoder;
            g_Instance->m_VideoDecoder = new pp::VideoDecoder(g_Instance);
        }
    }
    else {
        err = PP_ERROR_NOTSUPPORTED;
    }

    if (err == PP_ERROR_NOTSUPPORTED) {
        // Fallback to software decoding
        err = g_Instance->m_VideoDecoder->Initialize(
           g_Instance->m_Graphics3D,
           PP_VIDEOPROFILE_H264HIGH,
           PP_HARDWAREACCELERATION_NONE,
           0,
           pp::BlockUntilComplete());

        if (err == PP_ERROR_NOTSUPPORTED) {
            // No decoders available at all. We can't continue.
            ClDisplayMessage("No hardware or software H.264 decoders available!");
            g_Instance->StopConnection();
            return -1;
        }
        else if (!(drFlags & DR_FLAG_FORCE_SW_DECODE)) {
            // Tell the user we had to fall back
            ClDisplayTransientMessage("Hardware decoding is unavailable. Falling back to CPU decoding");
        }
    }
    
    pp::Module::Get()->core()->CallOnMainThread(0,
        g_Instance->m_CallbackFactory.NewCallback(&MoonlightInstance::DispatchGetPicture));
    
    return 0;
}

void MoonlightInstance::DispatchGetPicture(uint32_t unused) {
    // Queue the initial GetPicture callback on the main thread
    g_Instance->m_VideoDecoder->GetPicture(
        g_Instance->m_CallbackFactory.NewCallbackWithOutput(&MoonlightInstance::PictureReady));
}

void MoonlightInstance::VidDecCleanup(void) {
    if (g_Instance->m_UseNaClPlayer) {
        RunPlayerThreadWork(g_Instance->m_PlayerThread, false);
        free(s_DecodeBuffer);
        s_DecodeBuffer = NULL;
        s_DecodeBufferLength = 0;
        return;
    }

    free(s_DecodeBuffer);
    
    // Delete the decoder
    delete g_Instance->m_VideoDecoder;
    
    // Delete shader programs
    if (g_Instance->m_Texture2DShader.program) {
        glDeleteProgram(g_Instance->m_Texture2DShader.program);
        g_Instance->m_Texture2DShader.program = 0;
    }
    if (g_Instance->m_RectangleArbShader.program) {
        glDeleteProgram(g_Instance->m_RectangleArbShader.program);
        g_Instance->m_RectangleArbShader.program = 0;
    }
    if (g_Instance->m_ExternalOesShader.program) {
        glDeleteProgram(g_Instance->m_ExternalOesShader.program);
        g_Instance->m_ExternalOesShader.program = 0;
    }
    
    // Unbind graphics device by binding a default constructed object
    glSetCurrentContextPPAPI(0);
    g_Instance->m_Graphics3D = pp::Graphics3D();
    g_Instance->BindGraphics(g_Instance->m_Graphics3D);
}

static void WriteSpsNalu(PLENTRY nalu, unsigned char* outBuffer, unsigned int* offset) {
    int start_len = nalu->data[2] == 0x01 ? 3 : 4;
    h264_stream_t* stream = h264_new();
    
    // Read the old NALU
    read_nal_unit(stream,
                  (unsigned char *)&nalu->data[start_len],
                  nalu->length - start_len);
    
    if (!s_LoggedSps) {
        MoonlightInstance::ClLogMessage("SPS from host: profile=%d level=%d refs=%d vui=%d restriction=%d reorder=%d dec_buffering=%d\n",
                                        stream->sps->profile_idc, stream->sps->level_idc, stream->sps->num_ref_frames,
                                        stream->sps->vui_parameters_present_flag, stream->sps->vui.bitstream_restriction_flag,
                                        stream->sps->vui.num_reorder_frames, stream->sps->vui.max_dec_frame_buffering);
        s_LoggedSps = true;
    }

    // Fixup the SPS to what OS X needs to use hardware acceleration
    stream->sps->num_ref_frames = 1;

    // Declare that frames are never reordered and only one needs buffering.
    // Without bitstream restrictions the decoder must assume the worst case
    // and holds several frames before outputting each one, adding latency.
    stream->sps->vui_parameters_present_flag = 1;
    if (!stream->sps->vui.bitstream_restriction_flag) {
        stream->sps->vui.bitstream_restriction_flag = 1;
        stream->sps->vui.motion_vectors_over_pic_boundaries_flag = 1;
        stream->sps->vui.max_bytes_per_pic_denom = 2;
        stream->sps->vui.max_bits_per_mb_denom = 1;
        stream->sps->vui.log2_max_mv_length_horizontal = 16;
        stream->sps->vui.log2_max_mv_length_vertical = 16;
    }
    stream->sps->vui.num_reorder_frames = 0;
    stream->sps->vui.max_dec_frame_buffering = 1;
    
    // Copy the NALU prefix over from the original SPS
    memcpy(&outBuffer[*offset], nalu->data, start_len);
    *offset += start_len;
    
    // Copy the modified NALU data
    *offset += write_nal_unit(stream, &outBuffer[*offset], nalu->length + 32 - start_len);
    
    h264_free(stream);
}

int MoonlightInstance::VidDecSubmitDecodeUnit(PDECODE_UNIT decodeUnit) {
    PLENTRY entry;
    unsigned int offset;
    unsigned int totalLength;

    totalLength = decodeUnit->fullLength;
    if (decodeUnit->frameType == FRAME_TYPE_IDR) {
        // Add some extra space for the SPS fixup
        totalLength += 32;
    }
    
    // Resize the decode buffer if needed
    if (totalLength > s_DecodeBufferLength) {
        free(s_DecodeBuffer);
        s_DecodeBufferLength = totalLength;
        s_DecodeBuffer = (unsigned char *)malloc(s_DecodeBufferLength);
    }
    
    entry = decodeUnit->bufferList;
    offset = 0;
    while (entry != NULL) {
        if (entry->bufferType == BUFFER_TYPE_SPS) {
            // Write the SPS with required fixups and update offset
            WriteSpsNalu(entry, s_DecodeBuffer, &offset);
        }
        else {
            memcpy(&s_DecodeBuffer[offset], entry->data, entry->length);
            offset += entry->length;
        }
        
        entry = entry->next;
    }

    if (g_Instance->m_UseNaClPlayer) {
        if (s_LastPacketPts < 0.0) {
            s_FirstPresentationTimeMs = decodeUnit->presentationTimeMs;
        }

        NaClPlayer::ESPacket packet;
        packet.duration = 1.0 / s_PlayerFrameRate;
        // Stamp each frame with the player's own running time (time since Play()).
        // Host-relative timestamps start at 0 while the player clock has already
        // been running since setup, so every frame would arrive "late" and be dropped.
        uint64_t nowMs = LiGetMillis();
        packet.pts = (nowMs - s_PlayStartMs) / 1000.0;
        if (packet.pts <= s_LastPacketPts) {
            packet.pts = s_LastPacketPts + 0.001;
        }
        // Pad small frames with an H.264 filler-data NAL (type 12, ignored by decoders).
        // The player appears to batch input before decoding, so tiny frames (e.g. only the
        // mouse cursor moved) sit undisplayed until enough data accumulates.
        unsigned int padTarget = s_PadTargetBytes;
        if (padTarget > offset + 6) {
            if (padTarget > s_DecodeBufferLength) {
                unsigned char* grown = (unsigned char*)realloc(s_DecodeBuffer, padTarget);
                if (grown != NULL) {
                    s_DecodeBuffer = grown;
                    s_DecodeBufferLength = padTarget;
                }
            }
            if (padTarget <= s_DecodeBufferLength) {
                static const unsigned char fillerHeader[] = { 0x00, 0x00, 0x00, 0x01, 0x0C };
                memcpy(&s_DecodeBuffer[offset], fillerHeader, sizeof(fillerHeader));
                memset(&s_DecodeBuffer[offset + sizeof(fillerHeader)], 0xFF, padTarget - offset - sizeof(fillerHeader) - 1);
                s_DecodeBuffer[padTarget - 1] = 0x80; // rbsp_trailing_bits
                offset = padTarget;
            }
        }

        // Diagnostics: classify the frame by its first slice NAL (reference or not)
        for (unsigned int i = 0; i + 3 < offset; i++) {
            if (s_DecodeBuffer[i] == 0 && s_DecodeBuffer[i + 1] == 0 && s_DecodeBuffer[i + 2] == 1) {
                unsigned char nalType = s_DecodeBuffer[i + 3] & 0x1F;
                if (nalType == 1 || nalType == 5) {
                    if (((s_DecodeBuffer[i + 3] >> 5) & 0x3) == 0) {
                        s_DiagNonRefFrames++;
                    }
                    break;
                }
            }
        }
        s_DiagFrames++;
        if (offset < s_DiagMinBytes) s_DiagMinBytes = offset;
        if (offset > s_DiagMaxBytes) s_DiagMaxBytes = offset;

        if (nowMs >= s_NextClockLogMs) {
            NaClPlayer::TimeTicks clock = -1.0;
            int32_t clockResult = s_NaClPlayer->GetCurrentTime(clock);
            ClLogMessage("NaClPlayer: clock=%.3f (r=%d) pts=%.3f frames=%u nonref=%u bytes=%u..%u\n",
                         clock, clockResult, packet.pts, s_DiagFrames, s_DiagNonRefFrames,
                         s_DiagMinBytes, s_DiagMaxBytes);
            s_DiagFrames = s_DiagNonRefFrames = s_DiagMaxBytes = 0;
            s_DiagMinBytes = 0xFFFFFFFF;
            s_NextClockLogMs = nowMs + 1000;
        }
        s_LastPresentationTimeMs = decodeUnit->presentationTimeMs;
        s_LastPacketPts = packet.pts;
        packet.dts = packet.pts;
        packet.is_key_frame = decodeUnit->frameType == FRAME_TYPE_IDR;
        packet.size = offset;
        packet.buffer = s_DecodeBuffer;

        uint64_t submitStartMs = LiGetMillis();
        diag::BeginSubmit(submitStartMs);
        int32_t result = s_PlayerStream->AppendPacket(packet);
        uint64_t submitEndMs = LiGetMillis();
        diag::EndSubmit(offset, packet.is_key_frame,
                        decodeUnit->receiveTimeMs, decodeUnit->enqueueTimeMs,
                        decodeUnit->frameHostProcessingLatency,
                        submitStartMs, submitEndMs);
        if (result != NaClPlayer::ErrorCodes::Success) {
            if (submitEndMs >= s_NextAppendErrorLogMs) {
                ClLogMessage("NaClPlayer: AppendPacket=%d\n", result);
                s_NextAppendErrorLogMs = submitEndMs + 1000;
            }
            return DR_NEED_IDR;
        }
        return DR_OK;
    }
    
    // Start the decoding
    uint64_t submitStartMs = LiGetMillis();
    uint32_t decodeId = diag::BeginSubmit(submitStartMs);
    g_Instance->m_VideoDecoder->Decode(decodeId, offset, s_DecodeBuffer, pp::BlockUntilComplete());
    diag::EndSubmit(offset, decodeUnit->frameType == FRAME_TYPE_IDR,
                    decodeUnit->receiveTimeMs, decodeUnit->enqueueTimeMs,
                    decodeUnit->frameHostProcessingLatency,
                    submitStartMs, LiGetMillis());

    return DR_OK;
}

void MoonlightInstance::CreateShader(GLuint program, GLenum type,
                                     const char* source, int size) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, &size);
    glCompileShader(shader);
    glAttachShader(program, shader);
    glDeleteShader(shader);
}

Shader MoonlightInstance::CreateProgram(const char* vertexShader, const char* fragmentShader) {
    Shader shader;
    
    shader.program = glCreateProgram();
    CreateShader(shader.program, GL_VERTEX_SHADER, vertexShader, strlen(vertexShader));
    CreateShader(shader.program, GL_FRAGMENT_SHADER, fragmentShader, strlen(fragmentShader));
    glLinkProgram(shader.program);
    glUseProgram(shader.program);
    
    glUniform1i(glGetUniformLocation(shader.program, "s_texture"), 0);
    assertNoGLError();
    
    shader.texcoord_scale_location = glGetUniformLocation(shader.program, "v_scale");
    
    GLint pos_location = glGetAttribLocation(shader.program, "a_position");
    GLint tc_location = glGetAttribLocation(shader.program, "a_texCoord");
    assertNoGLError();
    
    glEnableVertexAttribArray( pos_location);
    glVertexAttribPointer(pos_location, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glEnableVertexAttribArray(tc_location);
    glVertexAttribPointer(tc_location, 2, GL_FLOAT, GL_FALSE, 0, static_cast<float*>(0) + 8);
    
    glUseProgram(0);
    assertNoGLError();
    return shader;
}

void MoonlightInstance::PaintPicture(void) {
    m_IsPainting = true;
    
    // Take the next picture into our ownership
    m_CurrentPicture = m_NextPicture;
    m_HasNextPicture = false;
    
    // Recycle bogus pictures immediately
    if (m_CurrentPicture.texture_target == 0) {
        m_VideoDecoder->RecyclePicture(m_CurrentPicture);
        m_IsPainting = false;
        return;
    }

    diag::OnPaintStart();

    // Calling glClear() once per frame is recommended for modern
    // GPUs which use it for state tracking hints.
    glClear(GL_COLOR_BUFFER_BIT);
    
    int originalTextureTarget = s_LastTextureType;
    
    // Only make these state changes if we've changed from the last texture type
    if (m_CurrentPicture.texture_target != s_LastTextureType) {
        if (m_CurrentPicture.texture_target == GL_TEXTURE_2D) {
            if (!g_Instance->m_Texture2DShader.program) {
                g_Instance->m_Texture2DShader = CreateProgram(k_VertexShader, k_FragmentShader2D);
            }
            glUseProgram(g_Instance->m_Texture2DShader.program);
            glUniform2f(g_Instance->m_Texture2DShader.texcoord_scale_location, 1.0, 1.0);
        }
        else if (m_CurrentPicture.texture_target == GL_TEXTURE_RECTANGLE_ARB) {
            if (!g_Instance->m_RectangleArbShader.program) {
                g_Instance->m_RectangleArbShader = CreateProgram(k_VertexShader, k_FragmentShaderRectangle);
            }
            glUseProgram(g_Instance->m_RectangleArbShader.program);
            glUniform2f(g_Instance->m_RectangleArbShader.texcoord_scale_location,
                        m_CurrentPicture.texture_size.width, m_CurrentPicture.texture_size.height);
        }
        else if (m_CurrentPicture.texture_target == GL_TEXTURE_EXTERNAL_OES) {
            if (!g_Instance->m_ExternalOesShader.program) {
                g_Instance->m_ExternalOesShader = CreateProgram(k_VertexShader, k_FragmentShaderExternal);
            }
            glUseProgram(g_Instance->m_ExternalOesShader.program);
            glUniform2f(g_Instance->m_ExternalOesShader.texcoord_scale_location, 1.0, 1.0);
        }

        glActiveTexture(GL_TEXTURE0);

        s_LastTextureType = m_CurrentPicture.texture_target;
    }
    
    // Only rebind our texture if we've changed since last time
    if (m_CurrentPicture.texture_id != s_LastTextureId || m_CurrentPicture.texture_target != originalTextureTarget) {
        glBindTexture(m_CurrentPicture.texture_target, m_CurrentPicture.texture_id);
        s_LastTextureId = m_CurrentPicture.texture_id;
    }
    
    // Draw the image
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    
    // Swap buffers
    m_Graphics3D.SwapBuffers(
        m_CallbackFactory.NewCallback(&MoonlightInstance::PaintFinished));
}

void MoonlightInstance::PaintFinished(int32_t result) {
    m_IsPainting = false;
    diag::OnPaintFinished();

    if (!s_FirstFrameDisplayed) {
        // Tell the JS code to display the video stream now
        pp::Var response("displayVideo");
        g_Instance->PostMessage(response);
        s_FirstFrameDisplayed = true;
    }
    
    ProfilerPrintDeltaFromNow("Paint -> Paint", s_LastPaintFinishedTime);
    s_LastPaintFinishedTime = ProfilerGetMillis();
    
    // Recycle the picture now that it's been painted
    uint64_t millis = ProfilerGetMillis();
    m_VideoDecoder->RecyclePicture(m_CurrentPicture);
    ProfilerPrintDeltaFromNow("RecyclePicture (PaintFinished)", millis);
    
    // Keep painting if we still have frames
    if (m_HasNextPicture) {
        PaintPicture();
    }
}

void MoonlightInstance::PictureReady(int32_t result, PP_VideoPicture picture) {
    if (result == PP_ERROR_ABORTED) {
        return;
    }
    
    diag::OnPictureReady(picture.decode_id, m_HasNextPicture);

    // Free a picture if there's one the renderer hasn't consumed yet
    if (m_HasNextPicture) {
        ProfilerPrintWarning("Decoder is outpacing renderer!");
        uint64_t millis = ProfilerGetMillis();
        m_VideoDecoder->RecyclePicture(m_NextPicture);
        ProfilerPrintDeltaFromNow("RecyclePicture (PictureReady)", millis);
    }
    
    // Put the latest picture in the slot for rendering next
    m_NextPicture = picture;
    m_HasNextPicture = true;
    
    // Queue another call to get another picture
    g_Instance->m_VideoDecoder->GetPicture(
        g_Instance->m_CallbackFactory.NewCallbackWithOutput(&MoonlightInstance::PictureReady));
    
    // Start painting if we aren't now
    if (!m_IsPainting) {
        PaintPicture();
    }
}

DECODER_RENDERER_CALLBACKS MoonlightInstance::s_DrCallbacks = {
    .setup = MoonlightInstance::VidDecSetup,
    .cleanup = MoonlightInstance::VidDecCleanup,
    .submitDecodeUnit = MoonlightInstance::VidDecSubmitDecodeUnit,
    .capabilities = CAPABILITY_SLICES_PER_FRAME(4)
};
