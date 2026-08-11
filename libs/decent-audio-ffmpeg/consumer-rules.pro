# R8 rules that travel inside the AAR (consumerProguardFiles), so a consuming
# app inherits them by depending on this module and cannot forget them.
#
# WHY THIS FILE HAS TO EXIST
# --------------------------
# The stock proguard-android-optimize.txt every app uses carries
#
#     -keepclasseswithmembernames class * { native <methods>; }
#
# which saves FlowyFfmpeg itself and the NAMES of its native methods — that is
# all JNI's automatic binding needs. It does NOT save anything the .so looks up
# by name at runtime: FindClass / GetMethodID targets, and constructors whose
# only caller is native code. R8 renames the first and strips the second
# outright, and the failure is invisible in debug and fatal on the first call
# in release.
#
# flowy_audio_jni.cc reaches into Java in exactly two places, and both are
# listed below:
#   * FindClass("com/decent/audio/FlowyFfmpeg$AudioInfo") + GetMethodID("<init>")
#     + NewObject, in probe(). AudioInfo's constructor has no Java caller at
#     all, so without a keep it does not survive shrinking.
#   * GetMethodID(onProgress) on whatever object implements ProgressListener,
#     in the progress callback.

# The entry point: native method names, and the constants/fields the native
# side and the caller share.
-keep class com.decent.audio.FlowyFfmpeg {
    *;
}

# Constructed by native code in probe(). Its fields are read only by the
# caller, but the constructor is invoked exclusively through NewObject.
-keep class com.decent.audio.FlowyFfmpeg$AudioInfo {
    *;
}

# Resolved by GetMethodID on the implementing class, so the interface method's
# name has to survive — and with it, the names of every override.
-keep interface com.decent.audio.FlowyFfmpeg$ProgressListener {
    *;
}
-keepclassmembers class * implements com.decent.audio.FlowyFfmpeg$ProgressListener {
    public void onProgress(long, long);
}
