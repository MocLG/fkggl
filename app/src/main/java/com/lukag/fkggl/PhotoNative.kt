package com.lukag.fkggl

import java.io.IOException

/** Thin JNI wrapper over photo.c's streaming fd API. */
object PhotoNative {

    init {
        System.loadLibrary("photo_jni")
    }

    /** Max payload bytes per PNG (599,999,992). */
    fun maxBytes(): Long = nativeMaxBytes()

    /** Encodes [len] bytes from [inFd] (at its current position) into a PNG written to [outFd].
     *  Returns the payload SHA-256 hex. Throws IOException on failure. */
    fun encodePart(inFd: Int, len: Long, outFd: Int): String = nativeEncodePart(inFd, len, outFd)

    /** Reads a whole PNG from [inFd], appends the extracted payload to [outFd] at its
     *  current position. Returns (sha256 hex, payload size). Throws IOException. */
    fun decodePart(inFd: Int, outFd: Int): Pair<String, Long> {
        val res = nativeDecodePart(inFd, outFd) ?: throw IOException("decode failed")
        return res[0] to res[1].toLong()
    }

    private external fun nativeMaxBytes(): Long
    private external fun nativeEncodePart(inFd: Int, len: Long, outFd: Int): String
    private external fun nativeDecodePart(inFd: Int, outFd: Int): Array<String>?
}
