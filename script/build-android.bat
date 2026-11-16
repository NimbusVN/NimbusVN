@echo off

cd platform\android

echo [1/1] Building Release APK (krkrsdl3 + openartemis)...
call gradlew.bat assembleRelease
echo[
echo APK: ..\..\out\android\app\outputs\apk\release\NimbusVN_v1.0.0.apk
