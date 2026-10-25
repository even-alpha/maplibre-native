// The JNI side of org.maplibre.android.cesium.CesiumTilesLayer: builds the native host that
// the Java CustomLayer takes over (CustomLayer's constructor adopts the pointer).

#include <mln/cesium/cesium_tiles_host.hpp>

#include <jni.h>

#include <string>

extern "C" JNIEXPORT jlong JNICALL Java_org_maplibre_android_cesium_CesiumTilesLayer_nativeCreateHost(
    JNIEnv* env, jclass, jstring tilesetUrl, jdouble maximumScreenSpaceError, jdouble heightOffsetMeters) {
    const char* chars = env->GetStringUTFChars(tilesetUrl, nullptr);
    std::string url(chars ? chars : "");
    if (chars) env->ReleaseStringUTFChars(tilesetUrl, chars);
    auto* host = new mln::cesium::CesiumTilesHost(std::move(url), maximumScreenSpaceError, heightOffsetMeters);
    return reinterpret_cast<jlong>(host);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_maplibre_android_cesium_CesiumTilesLayer_nativeIsLoading(JNIEnv*, jclass, jlong hostPointer) {
    auto* host = reinterpret_cast<mln::cesium::CesiumTilesHost*>(hostPointer);
    if (!mln::cesium::CesiumTilesHost::isAlive(host)) return JNI_FALSE;
    return host->isLoading() ? JNI_TRUE : JNI_FALSE;
}
