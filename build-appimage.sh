#!/bin/bash -e
# build-appimage.sh

LINUXDEPLOYQT_PATH="https://github.com/probonopd/linuxdeployqt/releases/download/continuous"
LINUXDEPLOYQT_FILE="linuxdeployqt-continuous-x86_64.AppImage"
LINUXDEPLOYQT_URL="${LINUXDEPLOYQT_PATH}/${LINUXDEPLOYQT_FILE}"

LINUXDEPLOY_PATH="https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous"
LINUXDEPLOY_FILE="linuxdeploy-x86_64.AppImage"
LINUXDEPLOY_URL="${LINUXDEPLOY_PATH}/${LINUXDEPLOY_FILE}"
APPDIR_BIN="./AppDir/usr/bin"

# Grab various appimage binaries from GitHub if we don't have them
if [ ! -e ./Tools/linuxdeploy ]; then
	wget ${LINUXDEPLOY_URL} -O ./Tools/linuxdeploy
	chmod +x ./Tools/linuxdeploy
fi
if [ ! -e ./Tools/linuxdeployqt ]; then
	wget ${LINUXDEPLOYQT_URL} -O ./Tools/linuxdeployqt
	chmod +x ./Tools/linuxdeployqt
fi
# Delete the AppDir folder to prevent build issues
# Delete the AppDir folder to prevent build issues
rm -rf ./AppDir/

# Build the AppDir directory for this image
mkdir -p AppDir
./Tools/linuxdeploy \
	--appdir=./AppDir \
	-e ./build/Binaries/dolphin-emu \
	-d ./Data/dolphin-emu.desktop \
	-i ./Data/dolphin-emu.png

# Add the Sys dir to the AppDir for packaging
cp -r Data/Sys ${APPDIR_BIN}
./Tools/linuxdeployqt ./AppDir/usr/share/applications/dolphin-emu.desktop -qmake=/opt/qt514/bin/qmake -appimage
mv *.AppImage Dolphin_REHD.AppImage
