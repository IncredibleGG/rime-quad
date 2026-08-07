# JNI 走 RegisterNatives，但 native 端仍會以名稱查 onDeployStatusFromNative。
-keepclasseswithmembernames class * {
    native <methods>;
}
-keep class org.rimequad.ime.core.RimeCore { *; }
