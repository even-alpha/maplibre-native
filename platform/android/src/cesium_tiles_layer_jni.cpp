// The JNI side of org.maplibre.android.cesium.CesiumTilesLayer: builds the native host that
// the Java CustomLayer takes over (CustomLayer's constructor adopts the pointer). Every other
// call goes through CesiumTilesHost::withLiveHost, since the style may have deleted the host.

#include <mln/cesium/cesium_tiles_host.hpp>

#include <jni.h>

#include <string>

namespace {

const mln::cesium::CesiumTilesHost* hostFrom(jlong hostPointer) {
    return reinterpret_cast<const mln::cesium::CesiumTilesHost*>(hostPointer);
}

} // namespace

extern "C" JNIEXPORT jlong JNICALL
Java_org_maplibre_android_cesium_CesiumTilesLayer_nativeCreateHost(JNIEnv* env,
                                                                   jclass,
                                                                   jstring tilesetUrl,
                                                                   jdouble maximumScreenSpaceError,
                                                                   jdouble heightOffsetMeters,
                                                                   jdouble minimumZoom,
                                                                   jdouble minimumPitchDegrees) {
    const char* chars = env->GetStringUTFChars(tilesetUrl, nullptr);
    std::string url(chars ? chars : "");
    if (chars) env->ReleaseStringUTFChars(tilesetUrl, chars);
    auto* host = new mln::cesium::CesiumTilesHost(
        std::move(url), maximumScreenSpaceError, heightOffsetMeters, minimumZoom, minimumPitchDegrees);
    return reinterpret_cast<jlong>(host);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_maplibre_android_cesium_CesiumTilesLayer_nativeIsLoading(JNIEnv*, jclass, jlong hostPointer) {
    bool loading = false;
    mln::cesium::CesiumTilesHost::withLiveHost(hostFrom(hostPointer),
                                               [&](mln::cesium::CesiumTilesHost& host) { loading = host.isLoading(); });
    return loading ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL Java_org_maplibre_android_cesium_CesiumTilesLayer_nativeSetEnabled(JNIEnv*,
                                                                                                    jclass,
                                                                                                    jlong hostPointer,
                                                                                                    jboolean enabled) {
    mln::cesium::CesiumTilesHost::withLiveHost(
        hostFrom(hostPointer), [&](mln::cesium::CesiumTilesHost& host) { host.setEnabled(enabled == JNI_TRUE); });
}

extern "C" JNIEXPORT void JNICALL Java_org_maplibre_android_cesium_CesiumTilesLayer_nativeTrimMemory(JNIEnv*,
                                                                                                    jclass,
                                                                                                    jlong hostPointer) {
    mln::cesium::CesiumTilesHost::withLiveHost(hostFrom(hostPointer),
                                               [](mln::cesium::CesiumTilesHost& host) { host.trimMemory(); });
}
