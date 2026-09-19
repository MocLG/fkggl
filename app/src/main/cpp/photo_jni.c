/* JNI bridge between PhotoNative.kt and the photo.c fd API. */
#include <jni.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "photo.h"

static jint throw_io(JNIEnv *env, const char *fallback)
{
    const char *msg = photo_last_error();
    jclass cls = (*env)->FindClass(env, "java/io/IOException");
    if (cls)
        (*env)->ThrowNew(env, cls, (msg && msg[0]) ? msg : fallback);
    return 0;
}

JNIEXPORT jlong JNICALL
Java_com_lukag_fkggl_PhotoNative_nativeMaxBytes(JNIEnv *env, jclass clazz)
{
    (void)env;
    (void)clazz;
    return (jlong)photo_max_bytes();
}

/* Returns the payload SHA-256 hex string, or throws IOException. */
JNIEXPORT jstring JNICALL
Java_com_lukag_fkggl_PhotoNative_nativeEncodePart(JNIEnv *env, jclass clazz,
                                                  jint in_fd, jlong len, jint out_fd)
{
    (void)clazz;
    char sha[65];
    if (photo_encode_fd((int)in_fd, (uint64_t)len, (int)out_fd, sha) != 0) {
        throw_io(env, "encode failed");
        return NULL;
    }
    return (*env)->NewStringUTF(env, sha);
}

/* Returns [sha_hex, byte_count] as strings, or throws IOException. */
JNIEXPORT jobjectArray JNICALL
Java_com_lukag_fkggl_PhotoNative_nativeDecodePart(JNIEnv *env, jclass clazz,
                                                  jint in_fd, jint out_fd)
{
    (void)clazz;
    char sha[65];
    char lenbuf[32];
    uint64_t len = 0;
    if (photo_decode_fd((int)in_fd, (int)out_fd, sha, &len) != 0) {
        throw_io(env, "decode failed");
        return NULL;
    }
    snprintf(lenbuf, sizeof lenbuf, "%llu", (unsigned long long)len);

    jclass strCls = (*env)->FindClass(env, "java/lang/String");
    if (!strCls)
        return NULL;
    jobjectArray arr = (*env)->NewObjectArray(env, 2, strCls, NULL);
    if (!arr)
        return NULL;
    (*env)->SetObjectArrayElement(env, arr, 0, (*env)->NewStringUTF(env, sha));
    (*env)->SetObjectArrayElement(env, arr, 1, (*env)->NewStringUTF(env, lenbuf));
    return arr;
}
