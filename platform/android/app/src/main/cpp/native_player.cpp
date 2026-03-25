// JNI bridge between Android Java/Kotlin and C++ player core.
// Provides native methods for PlayerActivity to control playback.

#include <jni.h>
#include <android/native_window_jni.h>
#include <android/log.h>
#include <string>
#include <memory>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

#include "common/log.h"
#include "common/packet_queue.h"
#include "common/frame_queue.h"
#include "demuxer/demuxer.h"
#include "decoder/decoder_factory.h"

#define JNI_FUNC(ret, name) \
    extern "C" JNIEXPORT ret JNICALL Java_com_sp_player_NativePlayer_##name

namespace {

struct AndroidPlayer {
    sp::PacketQueue video_pkt_queue{256};
    sp::PacketQueue audio_pkt_queue{256};
    sp::FrameQueue video_frame_queue{8};
    sp::FrameQueue audio_frame_queue{32};

    std::unique_ptr<sp::Demuxer> demuxer;
    std::unique_ptr<sp::IDecoder> video_decoder;
    std::unique_ptr<sp::IDecoder> audio_decoder;

    ANativeWindow* window = nullptr;
    bool playing = false;
    bool is_live = false;
};

AndroidPlayer* g_player = nullptr;

} // namespace

JNI_FUNC(jlong, nativeCreate)(JNIEnv* env, jobject thiz) {
    auto* player = new AndroidPlayer();
    SP_LOGI("JNI", "Player created");
    return reinterpret_cast<jlong>(player);
}

JNI_FUNC(jboolean, nativeOpen)(JNIEnv* env, jobject thiz, jlong handle, jstring url) {
    auto* player = reinterpret_cast<AndroidPlayer*>(handle);
    const char* url_str = env->GetStringUTFChars(url, nullptr);
    std::string url_cpp(url_str);
    env->ReleaseStringUTFChars(url, url_str);

    // Open demuxer
    player->demuxer = std::make_unique<sp::Demuxer>(
        player->video_pkt_queue, player->audio_pkt_queue);
    if (!player->demuxer->open(url_cpp)) {
        SP_LOGE("JNI", "Failed to open: %s", url_cpp.c_str());
        return JNI_FALSE;
    }

    const auto& info = player->demuxer->media_info();
    player->is_live = info.is_live;

    // Open video decoder (prefer MediaCodec hardware decoder on Android)
    if (info.video_codecpar) {
        player->video_decoder = sp::DecoderFactory::create(
            player->video_pkt_queue, player->video_frame_queue,
            info.video_codecpar, sp::DecoderType::Auto, "VDec");
        if (!player->video_decoder) return JNI_FALSE;
    }

    // Open audio decoder (software)
    if (info.audio_codecpar) {
        player->audio_decoder = sp::DecoderFactory::create(
            player->audio_pkt_queue, player->audio_frame_queue,
            info.audio_codecpar, sp::DecoderType::Software, "ADec");
        if (!player->audio_decoder) return JNI_FALSE;
    }

    SP_LOGI("JNI", "Opened: %s live=%s", url_cpp.c_str(), player->is_live ? "yes" : "no");
    return JNI_TRUE;
}

JNI_FUNC(void, nativeSetSurface)(JNIEnv* env, jobject thiz, jlong handle, jobject surface) {
    auto* player = reinterpret_cast<AndroidPlayer*>(handle);
    if (player->window) {
        ANativeWindow_release(player->window);
        player->window = nullptr;
    }
    if (surface) {
        player->window = ANativeWindow_fromSurface(env, surface);
        SP_LOGI("JNI", "Surface set: %p", player->window);
    }
}

JNI_FUNC(void, nativePlay)(JNIEnv* env, jobject thiz, jlong handle) {
    auto* player = reinterpret_cast<AndroidPlayer*>(handle);
    if (!player->demuxer) return;

    player->demuxer->start();
    if (player->video_decoder) player->video_decoder->start();
    if (player->audio_decoder) player->audio_decoder->start();
    player->playing = true;
    SP_LOGI("JNI", "Playback started");
}

JNI_FUNC(void, nativeStop)(JNIEnv* env, jobject thiz, jlong handle) {
    auto* player = reinterpret_cast<AndroidPlayer*>(handle);
    if (player->demuxer) player->demuxer->stop();
    if (player->video_decoder) player->video_decoder->stop();
    if (player->audio_decoder) player->audio_decoder->stop();

    player->video_pkt_queue.abort();
    player->audio_pkt_queue.abort();
    player->video_frame_queue.abort();
    player->audio_frame_queue.abort();
    player->playing = false;
    SP_LOGI("JNI", "Playback stopped");
}

JNI_FUNC(void, nativeDestroy)(JNIEnv* env, jobject thiz, jlong handle) {
    auto* player = reinterpret_cast<AndroidPlayer*>(handle);
    if (player->window) {
        ANativeWindow_release(player->window);
    }
    delete player;
    SP_LOGI("JNI", "Player destroyed");
}

JNI_FUNC(jboolean, nativeIsLive)(JNIEnv* env, jobject thiz, jlong handle) {
    auto* player = reinterpret_cast<AndroidPlayer*>(handle);
    return player->is_live ? JNI_TRUE : JNI_FALSE;
}
