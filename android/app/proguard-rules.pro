# Default ProGuard config for the JarvisNano companion app.
# Most of what we need is already covered by proguard-android-optimize.txt;
# this file holds project-specific keep rules.

# kotlinx.serialization — keep companion serializers and serializable classes.
-keepattributes *Annotation*, InnerClasses
-dontnote kotlinx.serialization.SerializationKt
-keep,includedescriptorclasses class com.ingeniousdigital.jarvisnano.**$$serializer { *; }
-keepclassmembers class com.ingeniousdigital.jarvisnano.** {
    *** Companion;
}
-keepclasseswithmembers class com.ingeniousdigital.jarvisnano.** {
    kotlinx.serialization.KSerializer serializer(...);
}

# OkHttp / Okio
-dontwarn okhttp3.**
-dontwarn okio.**
-dontwarn org.conscrypt.**

# jmdns
-dontwarn javax.jmdns.**
-keep class javax.jmdns.** { *; }
