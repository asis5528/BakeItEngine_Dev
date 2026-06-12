# BakeIt Quest build (Phase 1 skeleton)

Standalone Quest 3 APK: OpenXR + Vulkan, clear-color frame loop. Phase 2
brings the brick renderer from `../Source` in (see `Source/Platform/Platform.h`
for the porting rules).

## One-time setup

1. Install **Android Studio** (bundles JDK + SDK manager + gradle):
   https://developer.android.com/studio
2. In Android Studio: *SDK Manager -> SDK Tools*, check **NDK (side by side)**
   and **CMake**, apply.
3. On the headset: enable developer mode (Meta Horizon phone app ->
   headset -> Settings -> Developer Mode), connect USB-C, allow USB debugging
   in the headset when prompted.

## Build & deploy

Open this `Android/` folder in Android Studio (it generates the gradle
wrapper on first sync), then Run - or from a terminal once the wrapper
exists:

```
gradlew assembleDebug
adb install -r app\build\outputs\apk\debug\app-debug.apk
adb shell am start -n com.bakeit.questdemo/android.app.NativeActivity
```

Logs: `adb logcat -s BakeItQuest`

Success = the headset shows a slowly color-cycling void. That proves loader,
session, the OpenXR-created Vulkan device, swapchain, frame timing, and
composition - everything the engine needs underneath it.
