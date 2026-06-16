#!/bin/bash
export JAVA_HOME=/usr/lib/jvm/java-17-openjdk
if [ -d "$JAVA_HOME" ]; then
    echo "Using JAVA_HOME: $JAVA_HOME"
    export PATH=$JAVA_HOME/bin:$PATH
else
    echo "Error: Java 17 not found at $JAVA_HOME"
    exit 1
fi

# Accept licenses
SDK_MANAGER="/home/c0mplex/Android/Sdk/cmdline-tools/latest/bin/sdkmanager"
if [ -f "$SDK_MANAGER" ]; then
    yes | "$SDK_MANAGER" --licenses
fi

# Ensure gradlew exists
if [ ! -f "./gradlew" ]; then
    if [ -f "../gradlew" ]; then
        cp ../gradlew .
        cp ../gradlew.bat .
        cp -r ../gradle .
    else
        echo "gradlew not found. Attempting to generate..."
        gradle wrapper --gradle-version 8.5
    fi
    chmod +x gradlew
fi

echo "Setup complete."
