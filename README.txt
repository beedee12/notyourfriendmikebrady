SHIP EXPLORER STEREO FIX - GITHUB BUILD

You do NOT need Visual Studio, CMake, or any compiler on your PC.

1. Make a new EMPTY GitHub repository.
2. Upload EVERYTHING from this ZIP, including the .github folder.
   Easiest method on GitHub:
      Add file -> Upload files
      drag Plugin.cpp, README.txt and the .github folder contents into the repo
   IMPORTANT: GitHub's normal web uploader can be awkward with hidden folders.
   If it will not preserve .github/workflows/build.yml, create that path in GitHub manually:
      Add file -> Create new file
      filename: .github/workflows/build.yml
      paste the build.yml contents from this ZIP.
3. Open the repository's Actions tab.
4. Click "Build Ship Explorer Stereo Fix".
5. Click "Run workflow".
6. When the run finishes green, open it.
7. At the bottom under Artifacts download:
      ShipExplorerStereoFix
8. Unzip that artifact. It contains:
      ShipExplorerStereoFix.dll
9. Put the DLL here:
      %APPDATA%\UnrealVRMod\OceanLinerExplorer-Win64-Shipping\plugins\

USE THESE UEVR SETTINGS:
OpenVR
Extreme Compatibility ON
SceneView Compatibility ON
Synchronized Sequential
Skip Tick
Synchronization Mode: Very Late
Native Stereo Fix OFF
Native Stereo Fix Same Pass OFF
Split Screen Compatibility OFF

If the Action goes red, open the failed step and send ChatGPT the error text.
Do not start changing source or installing build software locally.
