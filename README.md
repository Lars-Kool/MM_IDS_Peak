# Micro-Manager Device Adapter for IDS Peak cameras
Micro-Manager is an application to control microscope hardware, such as cameras. It includes a hardware abstraction layer written in C++ and a user interface written in Java. Micro-Manager needs a translation layer between the driver (written by the manufacturer) and Micro-Manager's C++ backend. This translation layer is called a "Device Adapter" (Micro-Manager has chosen not to call it a "Driver" to distinguish it from the libraries provided by the manufacturers of the devices).

This GitHub repository contains a Device Adapter for IDS cameras. It contains both the already built .dll (mmgr_dal_IDSPeak.dll) and the C++/h files to build it yourself. Instructions can be found below. The Device Adapter was tested with an IDS USB3-3040CP-HQ Rev 2.2, and not with any other camera. Although this should not matter, there might be assumed default settings that might not be universally present.

## Using the precompiled .dll
The following steps will guide you through the process of "installing" the .dll, allowing you to use IDS cameras with Micro-Manager.
1. Clone/download this repository.
2. Download and install "IDS Peak" from the IDS website https://en.ids-imaging.com/downloads.html
3. Copy the "mmgr_dal_IDSPeak.dll" into the root folder of Micro-Manager (e.g. "C:\Program Files\Micro-Manager-2.0")
4. Copy all .dll and .lib files from the IDS Peak software into the root folder of Micro-Manager. The .dll and .lib files can be found in ".\PATH_TO_INSTALL\IDS\ids_peak\comfort_sdk\api\lib\x86_64" (.\PATH_TO_INSTALL is the folder you installed IDS Peak, e.g. "C:\Program Files").
5. Start Micro-Manager and either walk through the Hardware configuration wizard, or load the .cfg file included in this repository. Make sure the camera is plugged into a USB3 port before launching Micro-Manager.

## Building the device adapter yourself
More advanced users could build the device adapter themselves, allowing them to tailor the device adapter to their needs. If you just plan to use the default device adapter, there is no benefit to building it yourself. Below you will find a brief walkthrough on building the device adapter.
1. First, follow the Micro-Manager guide on building Micro-Manager (https://micro-manager.org/Building_MM_on_Windows) and on setting up a Visual Studio environment to building device adapters (https://micro-manager.org/Visual_Studio_project_settings_for_device_adapters).
2. In Visual Studio, right-click the project you created in step 1 and choose **Properties**. Under **Configuration Properties > C/C++ > General** add the following folder to the **Additional Include Directories**: ".\ids_peak\comfort_sdk\api\include". You can exit the **Properties** interface now.
3. Right-click the **Project** again, and click **Add > Existing Item**. Browse to ".\ids_peak\comfort_sdk\api\lib\x86_64" and add **"ids_peak_comfort_c.lib"**.
4. Right-click the **Header Files** tab under your project, and click **Add > Existing Item**, add the **"IDSPeak.h"** file.
5. Right-click the **Source Files** tab under your project, and click **Add > Existing Item**, add the **"IDSPeak.cpp"** file.
6. Right-click the **Project**, and click **Build**. The .dll will now be build, and should finish without any warnings/errors.
7. The .dll file can now be found in ".\micro-manager\mmCoreAndDevices\build\Debug\x64".
8. Now you have compiled the .dll, follow the steps under "Using the precompiled .dll" to enable Micro-Manager to communicate with IDS cameras.

## Known limitations
- **The maximum framerate of the 32bit RBGA pixel format is much lower than advertized or with IDS Peak Cockpit.**
  - This is indeed true, the problem is that the camera doesn't support recording BGRA8, which is the only accepted color format of Micro-Manager. Hence, the image has to be recorded in a different pixel format (in this case Bayer RG8) and then converted to BGRA8 on the fly. The maximum obtainable framerate then depends heavily on the (single core) processing speed of your PC. A potential solution is to not do the conversion (just pass the raw bayer data) and perform the debayering after all data is collected.
- **The minimum interval during the Multi-Dimensional Acquisition (MDA) is approximately 200 ms, even at low exposure times (e.g. 10 ms)**
  - I have no idea what functions are called by the MDA, and am thus not able to determine the rate limiting step. I will have to contact the developers of Micro-Manager to deepen my understanding of the MDA to be able to determine if we can improve this, and if so how.

## Future features
Better implementation of IDS auto white balance
More support for other pixel types (10/12 bit grayscale/color)
Recording Bayer / Packed images in RAW format (to post-process afterwards)
Give more meaningful error messages

## Acknowledgements
This Device Adapter was developed by Lars Kool at Institut Pierre-Gilles de Gennes (Paris, France).