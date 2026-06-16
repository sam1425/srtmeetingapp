#!/bin/bash
export JAVA_HOME=/usr/lib/jvm/java-17-openjdk
export PATH=$JAVA_HOME/bin:$PATH

if [ ! -f "./gradlew" ]; then
    echo "Error: gradlew not found. Run ./setup.sh first."
    exit 1
fi

echo "Building Debug APK..."
./gradlew assembleDebug

if [ $? -eq 0 ]; then
    APK_PATH="app/build/outputs/apk/debug/app-debug.apk"
    PACKAGE_NAME="com.meeting.srt"
    
    echo "Build successful!"
    
    # ADB Deployment
    if adb devices | grep -q "device$"; then
        echo "Device detected! Installing and launching..."
        adb install -r "$APK_PATH"
        adb shell am start -n "$PACKAGE_NAME/.MainActivity"
    else
        echo "No device found. Connect your phone via USB to auto-run."
        echo "APK: $APK_PATH"
    fi
else
    echo "Build failed."
    exit 1
fi
