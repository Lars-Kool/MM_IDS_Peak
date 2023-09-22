///////////////////////////////////////////////////////////////////////////////
// FILE:          IDSPeak.cpp
// PROJECT:       Micro-Manager
// SUBSYSTEM:     DeviceAdapters
//-----------------------------------------------------------------------------
// DESCRIPTION:   Driver for IDS peak series of USB cameras
//
//                Based on IDS peak SDK and Micro-manager DemoCamera example
//                tested with SDK version 2.5
//                Requires Micro-manager Device API 71 or higher!
//                
// AUTHOR:        Lars Kool, Institut Pierre-Gilles de Gennes
//
// YEAR:          2023
//                
// VERSION:       1.0
//
// LICENSE:       This file is distributed under the BSD license.
//                License text is included with the source distribution.
//
//                This file is distributed in the hope that it will be useful,
//                but WITHOUT ANY WARRANTY; without even the implied warranty
//                of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
//
//                IN NO EVENT SHALL THE COPYRIGHT OWNER OR
//                CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
//                INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES.
//
//LAST UPDATE:    21.09.2023 LK

#include <time.h>
#include "IDSPeak.h"
#include <cstdio>
#include <string>
#include <math.h>
#include "ModuleInterface.h"
#include <sstream>
#include <algorithm>
#include "WriteCompactTiffRGB.h"
#include <iostream>
#include <future>



using namespace std;
const double CIDSPeak::nominalPixelSizeUm_ = 1.0;
double g_IntensityFactor_ = 1.0;
const char* g_PixelType_8bit = "8bit";
const char* g_PixelType_32bitRGBA = "32bit RGBA";

// External names used used by the rest of the system
// to load particular device from the "IDSPeak.dll" library
const char* g_CameraDeviceName = "DCam";

///////////////////////////////////////////////////////////////////////////////
// Exported MMDevice API
///////////////////////////////////////////////////////////////////////////////

MODULE_API void InitializeModuleData()
{
    RegisterDevice(g_CameraDeviceName, MM::CameraDevice, "IDS camera");
}

MODULE_API MM::Device* CreateDevice(const char* deviceName)
{
    if (deviceName == 0)
        return 0;

    // decide which device class to create based on the deviceName parameter
    if (strcmp(deviceName, g_CameraDeviceName) == 0)
    {
        // create camera
        return new CIDSPeak();
    }

    // ...supplied name not recognized
    return 0;
}

MODULE_API void DeleteDevice(MM::Device* pDevice)
{
    delete pDevice;
}

///////////////////////////////////////////////////////////////////////////////
// CIDSPeak implementation
// ~~~~~~~~~~~~~~~~~~~~~~~~~~

/**
* CIDSPeak constructor.
* Setup default all variables and create device properties required to exist
* before intialization. In this case, no such properties were required. All
* properties will be created in the Initialize() method.
*
* As a general guideline Micro-Manager devices do not access hardware in the
* the constructor. We should do as little as possible in the constructor and
* perform most of the initialization in the Initialize() method.
*/
CIDSPeak::CIDSPeak() :
    CCameraBase<CIDSPeak> (),
    initialized_(false),
    readoutUs_(0.0),
    bitDepth_(8),
    roiX_(0),
    roiY_(0),
    roiMinSizeX_(0),
    roiMinSizeY_(0),
    roiInc_(1),
    sequenceStartTime_(0),
    isSequenceable_(false),
    sequenceMaxLength_(100),
    sequenceRunning_(false),
    sequenceIndex_(0),
    binSize_(1),
    cameraCCDXSize_(512),
    cameraCCDYSize_(512),
    ccdT_(0.0),
    triggerDevice_(""),
    stopOnOverflow_(false),
    supportsMultiROI_(false),
    multiROIFillValue_(0),
    nComponents_(1),
    exposureMax_(10000.0),
    exposureMin_(0.0),
    exposureInc_(1.0),
    exposureCur_(10.0),
    framerateCur_(10),
    framerateMax_(200),
    framerateMin_(0.1),
    framerateInc_(0.1),
    imageCounter_(0),
    gainMaster_(1.0),
    gainRed_(1.0),
    gainGreen_(1.0),
    gainBlue_(1.0)
{
    // call the base class method to set-up default error codes/messages
    InitializeDefaultErrorMessages();
    readoutStartTime_ = GetCurrentMMTime();
    thd_ = new MySequenceThread(this);
}

/**
* CIDSPeak destructor.
* If this device used as intended within the Micro-Manager system,
* Shutdown() will be always called before the destructor. But in any case
* we need to make sure that all resources are properly released even if
* Shutdown() was not called.
*/
CIDSPeak::~CIDSPeak()
{
    StopSequenceAcquisition();
    delete thd_;
}

/**
* Obtains device name.
* Required by the MM::Device API.
*/
void CIDSPeak::GetName(char* name) const
{
    // Return the name used to referr to this device adapte
    CDeviceUtils::CopyLimitedString(name, g_CameraDeviceName);
}

/**
* Intializes the hardware.
* Required by the MM::Device API.
* Typically we access and initialize hardware at this point.
* Device properties are typically created here as well, except
* the ones we need to use for defining initialization parameters.
* Such pre-initialization properties are created in the constructor.
* (This device does not have any pre-initialization properties)
*/
int CIDSPeak::Initialize()
{
    if (initialized_)
        return DEVICE_OK;

    // Initalize peak status
    status = PEAK_STATUS_SUCCESS;

    // Initialize peak library
    status = peak_Library_Init();
    if (status != PEAK_STATUS_SUCCESS) { return ERR_LIBRARY_NOT_INIT; }

    // update camera list
    status = peak_CameraList_Update(NULL);
    if (status != PEAK_STATUS_SUCCESS) { return ERR_CAMERA_NOT_FOUND; }

    // get length of camera list
    size_t cameraListLength = 0;
    status = peak_CameraList_Get(NULL, &cameraListLength);

    // exit program if no camera was found
    if (status != PEAK_STATUS_SUCCESS) { return ERR_CAMERA_NOT_FOUND; }

    // allocate memory for the camera list
    peak_camera_descriptor* cameraList = (peak_camera_descriptor*)calloc(
        cameraListLength, sizeof(peak_camera_descriptor));

    // get the camera list
    status = peak_CameraList_Get(cameraList, &cameraListLength);
    if (status != PEAK_STATUS_SUCCESS) { return ERR_CAMERA_NOT_FOUND; }

    // TODO: Let user pick camera, if multiple are available
    // select a camera to open
    size_t selectedCamera = 0;

    // open the selected camera
    status = peak_Camera_Open(cameraList[selectedCamera].cameraID, &hCam);
    if (status != PEAK_STATUS_SUCCESS) { return ERR_CAMERA_NOT_FOUND; }

    // free the camera list, not needed any longer
    free(cameraList);

    // check, which camera was actually opened
    peak_camera_descriptor cameraInfo;
    status = peak_Camera_GetDescriptor(peak_Camera_ID_FromHandle(hCam), &cameraInfo);
    if (status != PEAK_STATUS_SUCCESS) { return ERR_CAMERA_NOT_FOUND; }

    // set property list
    // -----------------

    // Name
    int nRet = CreateStringProperty(MM::g_Keyword_Name, g_CameraDeviceName, true);
    assert(nRet == DEVICE_OK);

    // Description
    nRet = CreateStringProperty(MM::g_Keyword_Description, "IDS Peak Camera Adapter", true);
    assert(nRet == DEVICE_OK);

    // CameraName
    nRet = CreateStringProperty(MM::g_Keyword_CameraName, cameraInfo.modelName, true);
    assert(nRet == DEVICE_OK);

    // CameraID
    char CamID[MM::MaxStrLength];
    _ui64toa_s(cameraInfo.cameraID, CamID, sizeof(CamID), 10);
    nRet = CreateStringProperty(MM::g_Keyword_CameraID, CamID, true);
    assert(nRet == DEVICE_OK);
    //free(CamID);

    // binning
    CPropertyAction* pAct = new CPropertyAction(this, &CIDSPeak::OnBinning);
    nRet = CreateIntegerProperty(MM::g_Keyword_Binning, 1, false, pAct);
    assert(nRet == DEVICE_OK);
    nRet = SetAllowedBinning();
    if (nRet != DEVICE_OK)
        return nRet;

    // pixel type
    status = peak_PixelFormat_Set(hCam, PEAK_PIXEL_FORMAT_MONO8);
    pAct = new CPropertyAction(this, &CIDSPeak::OnPixelType);
    nRet = CreateStringProperty(MM::g_Keyword_PixelType, g_PixelType_8bit, false, pAct);
    assert(nRet == DEVICE_OK);

    vector<string> pixelTypeValues;
    pixelTypeValues.push_back(g_PixelType_8bit);
    pixelTypeValues.push_back(g_PixelType_32bitRGBA);

    nRet = SetAllowedValues(MM::g_Keyword_PixelType, pixelTypeValues);
    if (nRet != DEVICE_OK)
        return nRet;

    // Exposure time, get range from camera (in us) and convert to ms
    double exposureTemp;
    status = peak_ExposureTime_Get(hCam, &exposureTemp);
    exposureCur_ = exposureTemp / 1000;
    nRet = CreateFloatProperty(MM::g_Keyword_Exposure, exposureCur_, false);
    assert(nRet == DEVICE_OK);
    status = peak_ExposureTime_GetRange(hCam, &exposureMin_, &exposureMax_, &exposureInc_);
    if (status != PEAK_STATUS_SUCCESS) { return ERR_DEVICE_NOT_AVAILABLE; }
    exposureMin_ /= 1000;
    exposureMax_ /= 1000;
    exposureInc_ /= 1000;
    nRet = SetPropertyLimits(MM::g_Keyword_Exposure, exposureMin_, exposureMax_);
    if (nRet != DEVICE_OK)
        return nRet;

    // Frame rate
    status = peak_FrameRate_GetRange(hCam, &framerateMin_, &framerateMax_, &framerateInc_);
    nRet = CreateFloatProperty("Maximum framerate", framerateMax_, false);
    assert(nRet == DEVICE_OK);
    nRet = CreateFloatProperty("Minimum framerate", framerateMin_, false);
    assert(nRet == DEVICE_OK);

    // Auto white balance
    initializeAutoWBConversion();
    status = peak_AutoWhiteBalance_Mode_Get(hCam, &peakAutoWhiteBalance_);
    pAct = new CPropertyAction(this, &CIDSPeak::OnAutoWhiteBalance);
    nRet = CreateStringProperty("Auto white balance", "Off", false, pAct);
    assert(nRet == DEVICE_OK);

    vector<string> autoWhiteBalanceValues;
    autoWhiteBalanceValues.push_back("Off");
    autoWhiteBalanceValues.push_back("Once");
    autoWhiteBalanceValues.push_back("Continuous");

    nRet = SetAllowedValues("Auto white balance", autoWhiteBalanceValues);
    if (nRet != DEVICE_OK)
        return nRet;

    // Gain master
    status = peak_Gain_GetRange(hCam, PEAK_GAIN_TYPE_DIGITAL, PEAK_GAIN_CHANNEL_MASTER, &gainMin_, &gainMax_, &gainInc_);
    status = peak_Gain_Get(hCam, PEAK_GAIN_TYPE_DIGITAL, PEAK_GAIN_CHANNEL_MASTER, &gainMaster_);
    pAct = new CPropertyAction(this, &CIDSPeak::OnGainMaster);
    nRet = CreateFloatProperty("Gain Master", 1.0, false, pAct);
    assert(nRet == DEVICE_OK);
    nRet = SetPropertyLimits("Gain Master", gainMin_, gainMax_);
    if (nRet != DEVICE_OK)
        return nRet;

    // Gain Red (should be set after gain master)
    status = peak_Gain_Get(hCam, PEAK_GAIN_TYPE_DIGITAL, PEAK_GAIN_CHANNEL_RED, &gainRed_);
    pAct = new CPropertyAction(this, &CIDSPeak::OnGainRed);
    nRet = CreateFloatProperty("Gain Red", gainRed_, false, pAct);
    assert(nRet == DEVICE_OK);
    nRet = SetPropertyLimits("Gain Red", gainMin_, gainMax_);
    if (nRet != DEVICE_OK)
        return nRet;

    // Gain Green (should be set after gain master)
    status = peak_Gain_Get(hCam, PEAK_GAIN_TYPE_DIGITAL, PEAK_GAIN_CHANNEL_GREEN, &gainGreen_);
    pAct = new CPropertyAction(this, &CIDSPeak::OnGainGreen);
    nRet = CreateFloatProperty("Gain Green", gainGreen_, false, pAct);
    assert(nRet == DEVICE_OK);
    nRet = SetPropertyLimits("Gain Green", gainMin_, gainMax_);
    if (nRet != DEVICE_OK)
        return nRet;

    //Gain Blue (should be called after gain master)
    status = peak_Gain_Get(hCam, PEAK_GAIN_TYPE_DIGITAL, PEAK_GAIN_CHANNEL_BLUE, &gainBlue_);
    pAct = new CPropertyAction(this, &CIDSPeak::OnGainBlue);
    nRet = CreateFloatProperty("Gain Blue", gainBlue_, false, pAct);
    assert(nRet == DEVICE_OK);
    nRet = SetPropertyLimits("Gain Blue", gainMin_, gainMax_);
    if (nRet != DEVICE_OK)
        return nRet;

    // camera temperature ReadOnly, and request camera temperature
    pAct = new CPropertyAction(this, &CIDSPeak::OnCCDTemp);
    nRet = CreateFloatProperty("CCDTemperature", 0, true, pAct);
    assert(nRet == DEVICE_OK);

    // readout time
    pAct = new CPropertyAction(this, &CIDSPeak::OnReadoutTime);
    nRet = CreateFloatProperty(MM::g_Keyword_ReadoutTime, 0, false, pAct);
    assert(nRet == DEVICE_OK);

    // CCD size of the camera we are modeling
    // getSensorInfo needs to be called before the CreateIntegerProperty
    // calls, othewise the default (512) values will be displayed.
    nRet = getSensorInfo();
    pAct = new CPropertyAction(this, &CIDSPeak::OnCameraCCDXSize);
    nRet = CreateIntegerProperty("OnCameraCCDXSize", 512, true, pAct);
    assert(nRet == DEVICE_OK);
    pAct = new CPropertyAction(this, &CIDSPeak::OnCameraCCDYSize);
    nRet = CreateIntegerProperty("OnCameraCCDYSize", 512, true, pAct);
    assert(nRet == DEVICE_OK);

    // Obtain ROI properties
    // The SetROI function used the CCD size, so this function should
    // always be put after the getSensorInfo call
    // It is assumed that the maximum ROI size is the size of the CCD
    // and that the increment in X and Y are identical
    peak_size roi_size_min;
    peak_size roi_size_max;
    peak_size roi_size_inc;
    status = peak_ROI_Size_GetRange(hCam, &roi_size_min, &roi_size_max, &roi_size_inc);
    if (status != PEAK_STATUS_SUCCESS) { return DEVICE_ERR; }
    roiMinSizeX_ = roi_size_min.width;
    roiMinSizeY_ = roi_size_min.height;
    roiInc_ = roi_size_inc.height;

    // Trigger device
    pAct = new CPropertyAction(this, &CIDSPeak::OnTriggerDevice);
    nRet = CreateStringProperty("TriggerDevice", "", false, pAct);
    assert(nRet == DEVICE_OK);

    pAct = new CPropertyAction(this, &CIDSPeak::OnSupportsMultiROI);
    nRet = CreateIntegerProperty("AllowMultiROI", 0, false, pAct);
    assert(nRet == DEVICE_OK);
    nRet = AddAllowedValue("AllowMultiROI", "0");
    assert(nRet == DEVICE_OK);
    nRet = AddAllowedValue("AllowMultiROI", "1");
    assert(nRet == DEVICE_OK);

    pAct = new CPropertyAction(this, &CIDSPeak::OnMultiROIFillValue);
    nRet = CreateIntegerProperty("MultiROIFillValue", 0, false, pAct);
    assert(nRet == DEVICE_OK);
    nRet = SetPropertyLimits("MultiROIFillValue", 0, 65536);
    assert(nRet == DEVICE_OK);

    // Whether or not to use exposure time sequencing
    pAct = new CPropertyAction(this, &CIDSPeak::OnIsSequenceable);
    std::string propName = "UseExposureSequences";
    nRet = CreateStringProperty(propName.c_str(), "No", false, pAct);
    assert(nRet == DEVICE_OK);
    nRet = AddAllowedValue(propName.c_str(), "Yes");
    assert(nRet == DEVICE_OK);
    nRet = AddAllowedValue(propName.c_str(), "No");
    assert(nRet == DEVICE_OK);

    // synchronize all properties
    // --------------------------
    nRet = UpdateStatus();
    if (nRet != DEVICE_OK)
        return nRet;
    
    // Debug framerate recording
    nRet = CreateFloatProperty("Interval", 0, false);
    assert(nRet == DEVICE_OK);

    // initialize image buffer
    GenerateEmptyImage(img_);

    // setup the buffer
    // This will set the buffer to the CCD size, not the ROI size,
    // hence the ROI needs to be cleared first.
    nRet = ClearROI();
    assert(nRet == DEVICE_OK);
    nRet = ResizeImageBuffer();
    if (nRet != DEVICE_OK)
        return nRet;

    initialized_ = true;
    return DEVICE_OK;
}

/**
* Shuts down (unloads) the device.
* Required by the MM::Device API.
* Ideally this method will completely unload the device and release all resources.
* Shutdown() may be called multiple times in a row.
* After Shutdown() we should be allowed to call Initialize() again to load the device
* without causing problems.
*/
int CIDSPeak::Shutdown()
{
    // Close open camera and set pointer to NULL
    (void)peak_Camera_Close(hCam);
    hCam = NULL;

    // Close peak library
    status = peak_Library_Exit();

    initialized_ = false;

    return DEVICE_OK;
}

/**
* Performs exposure and grabs a single image.
* This function should block during the actual exposure and return immediately afterwards
* (i.e., before readout).  This behavior is needed for proper synchronization with the shutter.
* Required by the MM::Camera API.
*/
int CIDSPeak::SnapImage()
{
    int nRet = DEVICE_OK;
    static int callCounter = 0;
    ++callCounter;

    MM::MMTime startTime = GetCurrentMMTime();

    unsigned int framesToAcquire = 1;
    unsigned int pendingFrames = framesToAcquire;
    unsigned int timeoutCount = 0;

    nRet = framerateSet(exposureCur_);
    uint32_t three_frame_times_timeout_ms = (uint32_t)((3000.0 / framerateCur_) + 0.5);

    status = peak_Acquisition_Start(hCam, framesToAcquire);
    if (status != PEAK_STATUS_SUCCESS) { return ERR_ACQ_START; }

    while (pendingFrames > 0)
    {
        peak_frame_handle hFrame;
        status = peak_Acquisition_WaitForFrame(hCam, three_frame_times_timeout_ms, &hFrame);
        if (status == PEAK_STATUS_TIMEOUT)
        {
            timeoutCount++;
            if (timeoutCount > 99) { return ERR_ACQ_TIMEOUT; }
            else { continue; }
        }
        else if (status == PEAK_STATUS_ABORTED) { break; }
        else if (status != PEAK_STATUS_SUCCESS) { return ERR_ACQ_FRAME; }

        // At this point we successfully got a frame handle. We can deal with the info now!
        nRet = transferBuffer(hFrame, img_);

        // Now we have transfered all information, we can release the frame.
        status = peak_Frame_Release(hCam, hFrame);
        if (PEAK_ERROR(status)) { return ERR_ACQ_RELEASE; }
        pendingFrames--;
    }

    if (peakAutoWhiteBalance_ != PEAK_AUTO_FEATURE_MODE_OFF)
    {
        updateAutoWhiteBalance();
    }
    readoutStartTime_ = GetCurrentMMTime();
    return nRet;
}


/**
* Returns pixel data.
* Required by the MM::Camera API.
* The calling program will assume the size of the buffer based on the values
* obtained from GetImageBufferSize(), which in turn should be consistent with
* values returned by GetImageWidth(), GetImageHight() and GetImageBytesPerPixel().
* The calling program also assumes that camera never changes the size of
* the pixel buffer on its own. In other words, the buffer can change only if
* appropriate properties are set (such as binning, pixel type, etc.)
*/
const unsigned char* CIDSPeak::GetImageBuffer()
{
    MMThreadGuard g(imgPixelsLock_);
    MM::MMTime readoutTime(readoutUs_);
    while (readoutTime > (GetCurrentMMTime() - readoutStartTime_)) {}
    unsigned char* pB = (unsigned char*)(img_.GetPixels());
    return pB;
}

/**
* Returns image buffer X-size in pixels.
* Required by the MM::Camera API.
*/
unsigned CIDSPeak::GetImageWidth() const
{
    return img_.Width();
}

/**
* Returns image buffer Y-size in pixels.
* Required by the MM::Camera API.
*/
unsigned CIDSPeak::GetImageHeight() const
{
    return img_.Height();
}

/**
* Returns image buffer pixel depth in bytes.
* Required by the MM::Camera API.
*/
unsigned CIDSPeak::GetImageBytesPerPixel() const
{
    return img_.Depth();
}

/**
* Returns the bit depth (dynamic range) of the pixel.
* This does not affect the buffer size, it just gives the client application
* a guideline on how to interpret pixel values.
* Required by the MM::Camera API.
*/
unsigned CIDSPeak::GetBitDepth() const
{
    return bitDepth_;
}

/**
* Returns the size in bytes of the image buffer.
* Required by the MM::Camera API.
*/
long CIDSPeak::GetImageBufferSize() const
{
    return img_.Width() * img_.Height() * GetImageBytesPerPixel();
}

/**
* Sets the camera Region Of Interest.
* Required by the MM::Camera API.
* This command will change the dimensions of the image.
* Depending on the hardware capabilities the camera may not be able to configure the
* exact dimensions requested - but should try do as close as possible.
* If both xSize and ySize are set to 0, the ROI is set to the entire CCD
* @param x - top-left corner coordinate
* @param y - top-left corner coordinate
* @param xSize - width
* @param ySize - height
*/
int CIDSPeak::SetROI(unsigned x, unsigned y, unsigned xSize, unsigned ySize)
{
    int ret = DEVICE_OK;
    if (peak_ROI_GetAccessStatus(hCam) == PEAK_ACCESS_READWRITE)
    {
        multiROIXs_.clear();
        multiROIYs_.clear();
        multiROIWidths_.clear();
        multiROIHeights_.clear();
        if (xSize == 0 && ySize == 0)
        {
            // effectively clear ROI
            int nRet = ResizeImageBuffer();
            if (nRet != DEVICE_OK) { return nRet; }
            roiX_ = 0;
            roiY_ = 0;
            xSize = cameraCCDXSize_;
            ySize = cameraCCDYSize_;
        }
        else
        {
            // If ROI is small than the minimum required size, set size to minimum
            if (xSize < roiMinSizeX_) { xSize = roiMinSizeX_; }
            if (ySize < roiMinSizeY_) { ySize = roiMinSizeY_; }
            // If ROI is not multiple of increment, reduce ROI such that it is
            xSize -= xSize % roiInc_;
            ySize -= ySize % roiInc_;
            // Check if ROI goes out of bounds, if so, push it in
            if (x + xSize > (unsigned int)cameraCCDXSize_) { x = cameraCCDXSize_ - xSize; }
            if (y + ySize > (unsigned int)cameraCCDYSize_) { y = cameraCCDYSize_ - ySize; }
            // apply ROI
            img_.Resize(xSize, ySize);
            roiX_ = x;
            roiY_ = y;
        }
        // Actually push the ROI settings to the camera
        peak_roi roi;
        roi.offset.x = roiX_;
        roi.offset.y = roiY_;
        roi.size.width = xSize;
        roi.size.height = ySize;
        status = peak_ROI_Set(hCam, roi);
    }
    else { return DEVICE_CAN_NOT_SET_PROPERTY; }
    return ret;
}

/**
* Returns the actual dimensions of the current ROI.
* If multiple ROIs are set, then the returned ROI should encompass all of them.
* Required by the MM::Camera API.
*/
int CIDSPeak::GetROI(unsigned& x, unsigned& y, unsigned& xSize, unsigned& ySize)
{
    x = roiX_;
    y = roiY_;

    xSize = img_.Width();
    ySize = img_.Height();

    return DEVICE_OK;
}

/**
* Resets the Region of Interest to full frame.
* Required by the MM::Camera API.
*/
int CIDSPeak::ClearROI()
{
    // Passing all zeros to SetROI sets the ROI to the full frame
    int nRet = SetROI(0, 0, 0, 0);
    return nRet;
}

/**
 * Queries if the camera supports multiple simultaneous ROIs.
 * Optional method in the MM::Camera API; by default cameras do not support
 * multiple ROIs.
 */
bool CIDSPeak::SupportsMultiROI()
{
    return supportsMultiROI_;
}

/**
 * Queries if multiple ROIs have been set (via the SetMultiROI method). Must
 * return true even if only one ROI was set via that method, but must return
 * false if an ROI was set via SetROI() or if ROIs have been cleared.
 * Optional method in the MM::Camera API; by default cameras do not support
 * multiple ROIs, so this method returns false.
 */
bool CIDSPeak::IsMultiROISet()
{
    return multiROIXs_.size() > 0;
}

/**
 * Queries for the current set number of ROIs. Must return zero if multiple
 * ROIs are not set (including if an ROI has been set via SetROI).
 * Optional method in the MM::Camera API; by default cameras do not support
 * multiple ROIs.
 */
int CIDSPeak::GetMultiROICount(unsigned int& count)
{
    count = (unsigned int)multiROIXs_.size();
    return DEVICE_OK;
}

/**
 * Set multiple ROIs. Replaces any existing ROI settings including ROIs set
 * via SetROI.
 * Optional method in the MM::Camera API; by default cameras do not support
 * multiple ROIs.
 * @param xs Array of X indices of upper-left corner of the ROIs.
 * @param ys Array of Y indices of upper-left corner of the ROIs.
 * @param widths Widths of the ROIs, in pixels.
 * @param heights Heights of the ROIs, in pixels.
 * @param numROIs Length of the arrays.
 */
int CIDSPeak::SetMultiROI(const unsigned int* xs, const unsigned int* ys,
    const unsigned* widths, const unsigned int* heights,
    unsigned numROIs)
{
    multiROIXs_.clear();
    multiROIYs_.clear();
    multiROIWidths_.clear();
    multiROIHeights_.clear();
    unsigned int minX = UINT_MAX;
    unsigned int minY = UINT_MAX;
    unsigned int maxX = 0;
    unsigned int maxY = 0;
    for (unsigned int i = 0; i < numROIs; ++i)
    {
        multiROIXs_.push_back(xs[i]);
        multiROIYs_.push_back(ys[i]);
        multiROIWidths_.push_back(widths[i]);
        multiROIHeights_.push_back(heights[i]);
        if (minX > xs[i])
        {
            minX = xs[i];
        }
        if (minY > ys[i])
        {
            minY = ys[i];
        }
        if (xs[i] + widths[i] > maxX)
        {
            maxX = xs[i] + widths[i];
        }
        if (ys[i] + heights[i] > maxY)
        {
            maxY = ys[i] + heights[i];
        }
    }
    img_.Resize(maxX - minX, maxY - minY);
    roiX_ = minX;
    roiY_ = minY;
    return DEVICE_OK;
}

/**
 * Queries for current multiple-ROI setting. May be called even if no ROIs of
 * any type have been set. Must return length of 0 in that case.
 * Optional method in the MM::Camera API; by default cameras do not support
 * multiple ROIs.
 * @param xs (Return value) X indices of upper-left corner of the ROIs.
 * @param ys (Return value) Y indices of upper-left corner of the ROIs.
 * @param widths (Return value) Widths of the ROIs, in pixels.
 * @param heights (Return value) Heights of the ROIs, in pixels.
 * @param numROIs Length of the input arrays. If there are fewer ROIs than
 *        this, then this value must be updated to reflect the new count.
 */
int CIDSPeak::GetMultiROI(unsigned* xs, unsigned* ys, unsigned* widths,
    unsigned* heights, unsigned* length)
{
    unsigned int roiCount = (unsigned int)multiROIXs_.size();
    if (roiCount > *length)
    {
        // This should never happen.
        return DEVICE_INTERNAL_INCONSISTENCY;
    }
    for (unsigned int i = 0; i < roiCount; ++i)
    {
        xs[i] = multiROIXs_[i];
        ys[i] = multiROIYs_[i];
        widths[i] = multiROIWidths_[i];
        heights[i] = multiROIHeights_[i];
    }
    *length = roiCount;
    return DEVICE_OK;
}

/**
* Returns the current exposure setting in milliseconds.
* Required by the MM::Camera API.
*/
double CIDSPeak::GetExposure() const
{
    char buf[MM::MaxStrLength];
    int nRet = GetProperty(MM::g_Keyword_Exposure, buf);
    if (nRet != DEVICE_OK) { return 0.; } // If something goes wrong, return 0. 
    return atof(buf);
}

/**
 * Returns the current exposure from a sequence and increases the sequence counter
 * Used for exposure sequences
 */
double CIDSPeak::GetSequenceExposure()
{
    if (exposureSequence_.size() == 0)
        return this->GetExposure();

    double exposure = exposureSequence_[sequenceIndex_];

    sequenceIndex_++;
    if (sequenceIndex_ >= exposureSequence_.size())
        sequenceIndex_ = 0;

    return exposure;
}

/**
* Sets exposure in milliseconds.
* Required by the MM::Camera API.
*/
void CIDSPeak::SetExposure(double exp)
{
    // Convert milliseconds to microseconds (peak cameras expect time in microseconds)
    // and make exposure set multiple of increment.
    double exposureSet = ceil(exp / exposureInc_) * exposureInc_ * 1000;
    // Check if we can write to the exposure time of the camera, if not do nothing
    if (peak_ExposureTime_GetAccessStatus(hCam) == PEAK_ACCESS_READWRITE)
    {
        // Check if exposure time is less than the minimun exposure time
        // If so, set it to minimum exposure time.
        if (exp <= exposureMin_) {
            printf("Exposure time too short. Exposure time set to minimum.");
            status = peak_ExposureTime_Set(hCam, exposureMin_ * 1000);
        }
        // Check if exposure time is less than the maximum exposure time
        // If so, set it to maximum exposure time.
        else if (exp >= exposureMax_) {
            printf("Exposure time too long. Exposure time set to maximum.");
            status = peak_ExposureTime_Set(hCam, exposureMax_ * 1000);
        }
        // 
        else
        {
            status = peak_ExposureTime_Set(hCam, exposureSet);
        }
        // Set displayed exposure time
        status = peak_ExposureTime_Get(hCam, &exposureCur_);
        SetProperty(MM::g_Keyword_Exposure, CDeviceUtils::ConvertToString(exposureCur_ / 1000));
        GetCoreCallback()->OnExposureChanged(this, exp);
    }
}

/**
* Returns the current binning factor.
* Required by the MM::Camera API.
*/
int CIDSPeak::GetBinning() const
{
    char buf[MM::MaxStrLength];
    int nRet = GetProperty(MM::g_Keyword_Binning, buf);
    if (nRet != DEVICE_OK) { return 0; } // If something goes wrong, return 0 (unphysical binning)
    return atoi(buf);
}

/**
* Sets binning factor.
* Required by the MM::Camera API.
*/
int CIDSPeak::SetBinning(int binF)
{
    if (peak_Binning_GetAccessStatus(hCam) == PEAK_ACCESS_READWRITE)
    {
        // Update binning
        status = peak_Binning_Set(hCam, (uint32_t)binF, (uint32_t)binF);
        if (status != PEAK_STATUS_SUCCESS) { return DEVICE_ERR; }
        binSize_ = binF;
        int ret = SetProperty(MM::g_Keyword_Binning, CDeviceUtils::ConvertToString(binF));

        // Update framerate range (since binning affects the maximum framerate)
        status = peak_FrameRate_GetRange(hCam, &framerateMin_, &framerateMax_, &framerateInc_);
        ret = SetProperty("Maximum framerate", CDeviceUtils::ConvertToString(framerateMax_));
        ret = SetProperty("Minimum framerate", CDeviceUtils::ConvertToString(framerateMin_));
        return ret;
    }
    else { return ERR_NO_WRITE_ACCESS; }
}

int CIDSPeak::IsExposureSequenceable(bool& isSequenceable) const
{
    isSequenceable = isSequenceable_;
    return DEVICE_OK;
}

int CIDSPeak::GetExposureSequenceMaxLength(long& nrEvents) const
{
    if (!isSequenceable_) {
        return DEVICE_UNSUPPORTED_COMMAND;
    }

    nrEvents = sequenceMaxLength_;
    return DEVICE_OK;
}

int CIDSPeak::StartExposureSequence()
{
    if (!isSequenceable_) {
        return DEVICE_UNSUPPORTED_COMMAND;
    }

    // may need thread lock
    sequenceRunning_ = true;
    return DEVICE_OK;
}

int CIDSPeak::StopExposureSequence()
{
    if (!isSequenceable_) {
        return DEVICE_UNSUPPORTED_COMMAND;
    }

    // may need thread lock
    sequenceRunning_ = false;
    sequenceIndex_ = 0;
    return DEVICE_OK;
}

/**
 * Clears the list of exposures used in sequences
 */
int CIDSPeak::ClearExposureSequence()
{
    if (!isSequenceable_) {
        return DEVICE_UNSUPPORTED_COMMAND;
    }

    exposureSequence_.clear();
    return DEVICE_OK;
}

/**
 * Adds an exposure to a list of exposures used in sequences
 */
int CIDSPeak::AddToExposureSequence(double exposureTime_ms)
{
    if (!isSequenceable_) {
        return DEVICE_UNSUPPORTED_COMMAND;
    }

    exposureSequence_.push_back(exposureTime_ms);
    return DEVICE_OK;
}

int CIDSPeak::SendExposureSequence() const {
    if (!isSequenceable_) {
        return DEVICE_UNSUPPORTED_COMMAND;
    }

    return DEVICE_OK;
}

int CIDSPeak::SetAllowedBinning()
{
    if (peak_Binning_GetAccessStatus(hCam) == PEAK_ACCESS_READONLY || peak_Binning_GetAccessStatus(hCam) == PEAK_ACCESS_READWRITE)
    {
        // Get the binning factors, uses two staged data query (first get length of list, then get list)
        size_t binningFactorCount;
        status = peak_Binning_FactorY_GetList(hCam, NULL, &binningFactorCount);
        if (status != PEAK_STATUS_SUCCESS) { return DEVICE_ERR; }
        uint32_t* binningFactorList = (uint32_t*)calloc(binningFactorCount, sizeof(uint32_t));
        status = peak_Binning_FactorY_GetList(hCam, binningFactorList, &binningFactorCount);
        if (status != PEAK_STATUS_SUCCESS) { return DEVICE_ERR; }

        vector<string> binValues;
        for (size_t i = 0; i < binningFactorCount; i++)
        {
            binValues.push_back(to_string(binningFactorList[i]));
        }
        LogMessage("Setting Allowed Binning settings", true);
        return SetAllowedValues(MM::g_Keyword_Binning, binValues);
    }
    else { return ERR_NO_READ_ACCESS; }
}


/**
 * Required by the MM::Camera API
 * Please implement this yourself and do not rely on the base class implementation
 * The Base class implementation is deprecated and will be removed shortly
 */
int CIDSPeak::StartSequenceAcquisition(double interval)
{
    return StartSequenceAcquisition(LONG_MAX, interval, false);
}

/**
* Stop and wait for the Sequence thread finished
*/
int CIDSPeak::StopSequenceAcquisition()
{
    if (!thd_->IsStopped()) {
        thd_->Stop();
        thd_->wait();
    }

    return DEVICE_OK;
}

/**
* Simple implementation of Sequence Acquisition
* A sequence acquisition should run on its own thread and transport new images
* coming of the camera into the MMCore circular buffer.
*/
int CIDSPeak::StartSequenceAcquisition(long numImages, double interval_ms, bool stopOnOverflow)
{
    if (IsCapturing())
    {
        return DEVICE_CAMERA_BUSY_ACQUIRING;
    }
    int nRet = DEVICE_OK;

    // Adjust framerate to match requested interval between frames
    nRet = framerateSet(interval_ms);

    // Wait until shutter is ready
    nRet = GetCoreCallback()->PrepareForAcq(this);
    if (nRet != DEVICE_OK)
        return nRet;
    sequenceStartTime_ = GetCurrentMMTime();
    imageCounter_ = 0;
    thd_->Start(numImages, interval_ms);
    stopOnOverflow_ = stopOnOverflow;
    return DEVICE_OK;
}

/*
 * Inserts Image and MetaData into MMCore circular Buffer
 */
int CIDSPeak::InsertImage()
{
    MM::MMTime timeStamp = this->GetCurrentMMTime();
    char label[MM::MaxStrLength];
    this->GetLabel(label);

    // Important:  metadata about the image are generated here:
    Metadata md;
    md.put("Camera", label);
    md.put(MM::g_Keyword_Elapsed_Time_ms, CDeviceUtils::ConvertToString((timeStamp - sequenceStartTime_).getMsec()));
    md.put(MM::g_Keyword_Metadata_ROI_X, CDeviceUtils::ConvertToString((long)roiX_));
    md.put(MM::g_Keyword_Metadata_ROI_Y, CDeviceUtils::ConvertToString((long)roiY_));

    char buf[MM::MaxStrLength];
    GetProperty(MM::g_Keyword_Binning, buf);
    md.put(MM::g_Keyword_Binning, buf);

    imageCounter_++;

    MMThreadGuard g(imgPixelsLock_);
    int nRet = GetCoreCallback()->InsertImage(this, img_.GetPixels(),
        img_.Width(),
        img_.Height(),
        img_.Depth(),
        md.Serialize().c_str());

    if (!stopOnOverflow_ && nRet == DEVICE_BUFFER_OVERFLOW)
    {
        // do not stop on overflow - just reset the buffer
        GetCoreCallback()->ClearImageBuffer(this);
        return GetCoreCallback()->InsertImage(this, img_.GetPixels(),
            img_.Width(),
            img_.Height(),
            img_.Depth(),
            md.Serialize().c_str());
    }
    else { return nRet; }
}

/*
 * Do actual capturing
 * Called from inside the thread
 */
int CIDSPeak::RunSequenceOnThread()
{
    int nRet = DEVICE_ERR;
    MM::MMTime startTime = GetCurrentMMTime();

    // Trigger
    if (triggerDevice_.length() > 0) {
        MM::Device* triggerDev = GetDevice(triggerDevice_.c_str());
        if (triggerDev != 0) {
            LogMessage("trigger requested");
            triggerDev->SetProperty("Trigger", "+");
        }
    }

    uint32_t three_frame_times_timeout_ms = (uint32_t)(3000 / framerateCur_ + 10);

    peak_frame_handle hFrame;
    status = peak_Acquisition_WaitForFrame(hCam, three_frame_times_timeout_ms, &hFrame);
    if (status != PEAK_STATUS_SUCCESS) { return DEVICE_ERR; }
    else { nRet = DEVICE_OK; }

    // At this point we successfully got a frame handle. We can deal with the info now!
    nRet = transferBuffer(hFrame, img_);
    if (nRet != DEVICE_OK) { return DEVICE_ERR; }
    else { nRet = DEVICE_OK; }

    nRet = InsertImage();
    if (nRet != DEVICE_OK) { return DEVICE_ERR; }
    else { nRet = DEVICE_OK; }

    // Now we have transfered all information, we can release the frame.
    status = peak_Frame_Release(hCam, hFrame);
    if (status != PEAK_STATUS_SUCCESS) { return DEVICE_ERR; }
    else { nRet = DEVICE_OK; }

    nRet = updateAutoWhiteBalance();

    return nRet;
};

bool CIDSPeak::IsCapturing() {
    return !thd_->IsStopped();
}

/*
 * called from the thread function before exit
 */
void CIDSPeak::OnThreadExiting() throw()
{
    try
    {
        LogMessage(g_Msg_SEQUENCE_ACQUISITION_THREAD_EXITING);
        GetCoreCallback() ? GetCoreCallback()->AcqFinished(this, 0) : DEVICE_OK;
    }
    catch (...)
    {
        LogMessage(g_Msg_EXCEPTION_IN_ON_THREAD_EXITING, false);
    }
}


MySequenceThread::MySequenceThread(CIDSPeak* pCam)
    :intervalMs_(default_intervalMS)
    , numImages_(default_numImages)
    , imageCounter_(0)
    , stop_(true)
    , suspend_(false)
    , camera_(pCam)
    , startTime_(0)
    , actualDuration_(0)
    , lastFrameTime_(0)
{};

MySequenceThread::~MySequenceThread() {};

void MySequenceThread::Stop() {
    MMThreadGuard g(this->stopLock_);
    stop_ = true;
}

void MySequenceThread::Start(long numImages, double intervalMs)
{
    MMThreadGuard g1(this->stopLock_);
    MMThreadGuard g2(this->suspendLock_);
    numImages_ = numImages;
    intervalMs_ = intervalMs;
    imageCounter_ = 0;
    stop_ = false;
    suspend_ = false;
    activate();
    actualDuration_ = MM::MMTime{};
    startTime_ = camera_->GetCurrentMMTime();
    lastFrameTime_ = MM::MMTime{};
}

//void MySequenceThread::SetIntervalMs(double intervalms)
//{
//    intervalMs_ = intervalms;
//}

bool MySequenceThread::IsStopped() {
    MMThreadGuard g(this->stopLock_);
    return stop_;
}

void MySequenceThread::Suspend() {
    MMThreadGuard g(this->suspendLock_);
    suspend_ = true;
}

bool MySequenceThread::IsSuspended() {
    MMThreadGuard g(this->suspendLock_);
    return suspend_;
}

void MySequenceThread::Resume() {
    MMThreadGuard g(this->suspendLock_);
    suspend_ = false;
}

int MySequenceThread::svc(void) throw()
{
    int nRet = DEVICE_ERR;
    peak_status status = PEAK_STATUS_SUCCESS;
    try
    {
        // peak_Acquisition_Start doesn't take LONG_MAX (2.1B) as near infinite, it crashes.
        // Instead, if numImages is LONG_MAX, PEAK_INFINITE is passed. This means that sometimes
        // the acquisition has to be stopped manually, but since this is properly escaped anyway
        // (in case of manual closing live view), this is all handled.
        if (numImages_ == LONG_MAX) { status = peak_Acquisition_Start(camera_->hCam, PEAK_INFINITE); }
        else { status = peak_Acquisition_Start(camera_->hCam, (uint32_t)numImages_); }

        // Check if acquisition is started properly
        if (status != PEAK_STATUS_SUCCESS) { return ERR_ACQ_START; }

        // do-while loop over numImages_
        do
        {
            nRet = camera_->RunSequenceOnThread();
        } while (nRet == DEVICE_OK && !IsStopped() && imageCounter_++ < numImages_ - 1);

        // If the acquisition is stopped manually, the acquisition has to be properly closed to
        // prevent the camera to be locked in acquisition mode.
        if (IsStopped())
        {
            status = peak_Acquisition_Stop(camera_->hCam);
            camera_->LogMessage("SeqAcquisition interrupted by the user\n");
        }
    }
    catch (...) {
        camera_->LogMessage(g_Msg_EXCEPTION_IN_THREAD, false);
    }
    stop_ = true;
    actualDuration_ = camera_->GetCurrentMMTime() - startTime_;
    camera_->OnThreadExiting();
    return nRet;
}


///////////////////////////////////////////////////////////////////////////////
// CIDSPeak Action handlers
///////////////////////////////////////////////////////////////////////////////

int CIDSPeak::OnMaxExposure(MM::PropertyBase* pProp, MM::ActionType eAct)
{
    if (eAct == MM::BeforeGet)
    {
        pProp->Set(exposureMax_);
        return DEVICE_OK;
    }
    else if (eAct == MM::AfterSet)
    {
        if (IsCapturing())
            return DEVICE_CAMERA_BUSY_ACQUIRING;

        double exposureSet;
        pProp->Get(exposureSet);

        if (peak_ExposureTime_GetAccessStatus(hCam) == PEAK_ACCESS_READWRITE)
        {
            status = peak_ExposureTime_Set(hCam, exposureMax_);
            if (status != PEAK_STATUS_SUCCESS) { return DEVICE_ERR; } // Should not be possible
            status = peak_ExposureTime_Get(hCam, &exposureCur_);
            if (status != PEAK_STATUS_SUCCESS) { return DEVICE_ERR; } // Should not be possible
            exposureCur_ /= 1000;
            int nRet = SetProperty(MM::g_Keyword_Exposure, CDeviceUtils::ConvertToString(exposureCur_));
            GetCoreCallback()->OnExposureChanged(this, exposureCur_);
            return nRet;
        }
        else { return ERR_NO_WRITE_ACCESS; }
    }
    return DEVICE_OK; // Should not be possible, but doesn't affect anything
}

/**
* Handles "Binning" property.
*/
int CIDSPeak::OnBinning(MM::PropertyBase* pProp, MM::ActionType eAct)
{
    int nRet = DEVICE_ERR;
    switch (eAct)
    {
    case MM::AfterSet:
    {
        if (IsCapturing())
            return DEVICE_CAMERA_BUSY_ACQUIRING;

        // the user just set the new value for the property, so we have to
        // apply this value to the 'hardware'.
        long binFactor;
        pProp->Get(binFactor);
        if (binFactor > 0 && binFactor < 10)
        {
            // calculate ROI using the previous bin settings
            double factor = (double)binFactor / (double)binSize_;
            roiX_ = (unsigned int)(roiX_ / factor);
            roiY_ = (unsigned int)(roiY_ / factor);
            for (unsigned int i = 0; i < multiROIXs_.size(); ++i)
            {
                multiROIXs_[i] = (unsigned int)(multiROIXs_[i] / factor);
                multiROIYs_[i] = (unsigned int)(multiROIYs_[i] / factor);
                multiROIWidths_[i] = (unsigned int)(multiROIWidths_[i] / factor);
                multiROIHeights_[i] = (unsigned int)(multiROIHeights_[i] / factor);
            }
            img_.Resize(
                (unsigned int)(img_.Width() / factor),
                (unsigned int)(img_.Height() / factor)
            );
            binSize_ = binFactor;
            std::ostringstream os;
            os << binSize_;
            OnPropertyChanged("Binning", os.str().c_str());
            nRet = DEVICE_OK;
        }
    }break;
    case MM::BeforeGet:
    {
        nRet = DEVICE_OK;
        pProp->Set(binSize_);
    }break;
    default:
        break;
    }
    return nRet;
}

/**
* Handles "Auto whitebalance" property.
*/
int CIDSPeak::OnAutoWhiteBalance(MM::PropertyBase* pProp, MM::ActionType eAct)
{
    int nRet = DEVICE_OK;

    if (eAct == MM::BeforeGet)
    {
        if (peak_AutoWhiteBalance_GetAccessStatus(hCam) == PEAK_ACCESS_READWRITE
            || peak_AutoWhiteBalance_GetAccessStatus(hCam) == PEAK_ACCESS_READONLY)
        {
            status = peak_AutoWhiteBalance_Mode_Get(hCam, &peakAutoWhiteBalance_);
        }
        const char* autoWB = peakAutoToString[peakAutoWhiteBalance_].c_str();
        pProp->Set(autoWB);
    }

    else if (eAct == MM::AfterSet)
    {
        if (IsCapturing())
            return DEVICE_CAMERA_BUSY_ACQUIRING;

        string autoWB;
        pProp->Get(autoWB);

        status = peak_AutoWhiteBalance_Mode_Set(hCam, (peak_auto_feature_mode)stringToPeakAuto[autoWB]);
        if (status != PEAK_STATUS_SUCCESS) { nRet = ERR_NO_WRITE_ACCESS; }
        else { peakAutoWhiteBalance_ = (peak_auto_feature_mode)stringToPeakAuto[autoWB]; }
    }
    return nRet;
}

/**
* Handles "Gain master" property.
*/
int CIDSPeak::OnGainMaster(MM::PropertyBase* pProp, MM::ActionType eAct)
{

    int nRet = DEVICE_OK;

    if (eAct == MM::BeforeGet)
    {

        pProp->Set(gainMaster_);
    }

    else if (eAct == MM::AfterSet)
    {
        if (IsCapturing())
            return DEVICE_CAMERA_BUSY_ACQUIRING;

        double gainMaster;
        pProp->Get(gainMaster);

        status = peak_Gain_Set(hCam, PEAK_GAIN_TYPE_DIGITAL, PEAK_GAIN_CHANNEL_RED, gainMaster);
        if (status != PEAK_STATUS_SUCCESS) { nRet = ERR_NO_WRITE_ACCESS; }
        else { gainMaster_ = gainMaster; }
    }
    return nRet;
}

/**
* Handles "Gain red" property.
*/
int CIDSPeak::OnGainRed(MM::PropertyBase* pProp, MM::ActionType eAct)
{

    int nRet = DEVICE_OK;

    if (eAct == MM::BeforeGet) { pProp->Set(gainRed_); }

    else if (eAct == MM::AfterSet)
    {
        if (IsCapturing())
            return DEVICE_CAMERA_BUSY_ACQUIRING;

        double gainRed;
        pProp->Get(gainRed);

        status = peak_Gain_Set(hCam, PEAK_GAIN_TYPE_DIGITAL, PEAK_GAIN_CHANNEL_RED, gainRed);
        if (status != PEAK_STATUS_SUCCESS) { nRet = ERR_NO_WRITE_ACCESS; }
        else { gainRed_ = gainRed; }

    }
    return nRet;
}

/**
* Handles "Gain green" property.
*/
int CIDSPeak::OnGainGreen(MM::PropertyBase* pProp, MM::ActionType eAct)
{

    int nRet = DEVICE_OK;

    if (eAct == MM::BeforeGet) { pProp->Set(gainGreen_); }

    else if (eAct == MM::AfterSet)
    {
        if (IsCapturing())
            return DEVICE_CAMERA_BUSY_ACQUIRING;

        double gainGreen;
        pProp->Get(gainGreen);

        status = peak_Gain_Set(hCam, PEAK_GAIN_TYPE_DIGITAL, PEAK_GAIN_CHANNEL_GREEN, gainGreen);
        if (status != PEAK_STATUS_SUCCESS) { nRet = ERR_NO_WRITE_ACCESS; }
        else { gainGreen_ = gainGreen; }

    }
    return nRet;
}

/**
* Handles "Gain blue" property.
*/
int CIDSPeak::OnGainBlue(MM::PropertyBase* pProp, MM::ActionType eAct)
{

    int nRet = DEVICE_OK;

    if (eAct == MM::BeforeGet) { pProp->Set(gainBlue_); }

    else if (eAct == MM::AfterSet)
    {
        if (IsCapturing())
            return DEVICE_CAMERA_BUSY_ACQUIRING;

        double gainBlue;
        pProp->Get(gainBlue);

        status = peak_Gain_Set(hCam, PEAK_GAIN_TYPE_DIGITAL, PEAK_GAIN_CHANNEL_BLUE, gainBlue);
        if (status != PEAK_STATUS_SUCCESS) { nRet = ERR_NO_WRITE_ACCESS; }
        else { gainBlue_ = gainBlue; }

    }
    return nRet;
}

/**
* Handles "PixelType" property.
*/
int CIDSPeak::OnPixelType(MM::PropertyBase* pProp, MM::ActionType eAct)
{
    int nRet = DEVICE_OK;
    switch (eAct)
    {
    case MM::AfterSet:
    {
        if (IsCapturing())
            return DEVICE_CAMERA_BUSY_ACQUIRING;

        string pixelType;
        pProp->Get(pixelType);

        if (peak_PixelFormat_GetAccessStatus(hCam) == PEAK_ACCESS_READWRITE)
        {
            if (pixelType == g_PixelType_8bit)
            {
                status = peak_PixelFormat_Set(hCam, PEAK_PIXEL_FORMAT_MONO8);
                nComponents_ = 1;
            }
            else
            {
                status = peak_PixelFormat_Set(hCam, PEAK_PIXEL_FORMAT_BAYER_RG8);
                nComponents_ = 4;
            }
        }
        else
        {
            return ERR_NO_WRITE_ACCESS;
        }

        // Only 8bit formats are supported for now
        bitDepth_ = 8;

        // Resize buffer to accomodate the new image
        img_.Resize(img_.Width(), img_.Height(), nComponents_ * (bitDepth_ / 8));
        nRet = DEVICE_OK;        
    }
    break;
    case MM::BeforeGet:
    {
        if (nComponents_ == 1)
        {
            pProp->Set(g_PixelType_8bit);
        }
        else
        {
            pProp->Set(g_PixelType_32bitRGBA);
        }
       
    } break;
    default:
        break;
    }
    return nRet;
}

/**
* Handles "ReadoutTime" property.
* Not sure this does anything...
*/
int CIDSPeak::OnReadoutTime(MM::PropertyBase* pProp, MM::ActionType eAct)
{
    if (eAct == MM::AfterSet)
    {
        double readoutMs;
        pProp->Get(readoutMs);

        readoutUs_ = readoutMs * 1000.0;
    }
    else if (eAct == MM::BeforeGet)
    {
        pProp->Set(readoutUs_ / 1000.0);
    }

    return DEVICE_OK;
}

int CIDSPeak::OnSupportsMultiROI(MM::PropertyBase* pProp, MM::ActionType eAct)
{
    if (eAct == MM::AfterSet)
    {
        long tvalue = 0;
        pProp->Get(tvalue);
        supportsMultiROI_ = (tvalue != 0);
    }
    else if (eAct == MM::BeforeGet)
    {
        pProp->Set((long)supportsMultiROI_);
    }

    return DEVICE_OK;
}

int CIDSPeak::OnMultiROIFillValue(MM::PropertyBase* pProp, MM::ActionType eAct)
{
    if (eAct == MM::AfterSet)
    {
        long tvalue = 0;
        pProp->Get(tvalue);
        multiROIFillValue_ = (int)tvalue;
    }
    else if (eAct == MM::BeforeGet)
    {
        pProp->Set((long)multiROIFillValue_);
    }

    return DEVICE_OK;
}

int CIDSPeak::OnCameraCCDXSize(MM::PropertyBase* pProp, MM::ActionType eAct)
{
    if (eAct == MM::BeforeGet)
    {
        pProp->Set(cameraCCDXSize_);
    }
    else if (eAct == MM::AfterSet)
    {
        long value;
        pProp->Get(value);
        if ((value < 16) || (33000 < value))
            return DEVICE_ERR;  // invalid image size
        if (value != cameraCCDXSize_)
        {
            cameraCCDXSize_ = value;
            img_.Resize(cameraCCDXSize_ / binSize_, cameraCCDYSize_ / binSize_);
        }
    }
    return DEVICE_OK;

}

int CIDSPeak::OnCameraCCDYSize(MM::PropertyBase* pProp, MM::ActionType eAct)
{
    if (eAct == MM::BeforeGet)
    {
        pProp->Set(cameraCCDYSize_);
    }
    else if (eAct == MM::AfterSet)
    {
        long value;
        pProp->Get(value);
        if ((value < 16) || (33000 < value))
            return DEVICE_ERR;  // invalid image size
        if (value != cameraCCDYSize_)
        {
            cameraCCDYSize_ = value;
            img_.Resize(cameraCCDXSize_ / binSize_, cameraCCDYSize_ / binSize_);
        }
    }
    return DEVICE_OK;

}

int CIDSPeak::OnTriggerDevice(MM::PropertyBase* pProp, MM::ActionType eAct)
{
    if (eAct == MM::BeforeGet)
    {
        pProp->Set(triggerDevice_.c_str());
    }
    else if (eAct == MM::AfterSet)
    {
        pProp->Get(triggerDevice_);
    }
    return DEVICE_OK;
}


int CIDSPeak::OnCCDTemp(MM::PropertyBase* pProp, MM::ActionType eAct)
{
    // This is a readonly function
    if (eAct == MM::BeforeGet)
    {
        status = getTemperature(&ccdT_);
        pProp->Set(ccdT_);
    }
    return DEVICE_OK;
}

int CIDSPeak::OnIsSequenceable(MM::PropertyBase* pProp, MM::ActionType eAct)
{
    std::string val = "Yes";
    if (eAct == MM::BeforeGet)
    {
        if (!isSequenceable_)
        {
            val = "No";
        }
        pProp->Set(val.c_str());
    }
    else if (eAct == MM::AfterSet)
    {
        isSequenceable_ = false;
        pProp->Get(val);
        if (val == "Yes")
        {
            isSequenceable_ = true;
        }
    }

    return DEVICE_OK;
}

///////////////////////////////////////////////////////////////////////////////
// Private CIDSPeak methods
///////////////////////////////////////////////////////////////////////////////

/**
* Sync internal image buffer size to the chosen property values.
*/
int CIDSPeak::ResizeImageBuffer()
{
    char buf[MM::MaxStrLength];
    int nRet = GetProperty(MM::g_Keyword_Binning, buf);
    if (nRet != DEVICE_OK)
        return nRet;
    binSize_ = atol(buf);

    img_.Resize(cameraCCDXSize_ / binSize_, cameraCCDYSize_ / binSize_, nComponents_ * (bitDepth_/8));
    return DEVICE_OK;
}

void CIDSPeak::GenerateEmptyImage(ImgBuffer& img)
{
    MMThreadGuard g(imgPixelsLock_);
    if (img.Height() == 0 || img.Width() == 0 || img.Depth() == 0)
        return;
    unsigned char* pBuf = const_cast<unsigned char*>(img.GetPixels());
    memset(pBuf, 0, img.Height() * img.Width() * img.Depth());
}

peak_status CIDSPeak::getTemperature(double* sensorTemp)
{
    size_t enumerationEntryCount = 0;

    if (PEAK_IS_READABLE(
        peak_GFA_Feature_GetAccessStatus(hCam, PEAK_GFA_MODULE_REMOTE_DEVICE, "DeviceFirmwareVersion")))
    {
        // get the length of the feature string
        status = peak_GFA_Enumeration_GetList(hCam, PEAK_GFA_MODULE_REMOTE_DEVICE, "DeviceTemperatureSelector", NULL, &enumerationEntryCount);
        status = getGFAfloat("DeviceTemperature", sensorTemp);
    }
    else
    {
        printf("No read access to device temperature");
    }
    return status;
}

int CIDSPeak::cleanExit()
{

    // Clean up before exit
    // Stop acquisition, if running
    if (peak_Acquisition_IsStarted(hCam))
    {
        // Stop acquisition
        status = peak_Acquisition_Stop(hCam);
        checkForSuccess(status, PEAK_TRUE);
    }

    // Close camera, if open
    if (hCam != PEAK_INVALID_HANDLE)
    {
        // Close Camera
        status = peak_Camera_Close(hCam);
        checkForSuccess(status, PEAK_TRUE);
    }

    // Exit library
    status = peak_Library_Exit();
    checkForSuccess(status, PEAK_TRUE);

    return status;
}

//Returns PEAK_TRUE, if function was successful.
//Returns PEAK_FALSE, if function returned with an error. If continueExecution == PEAK_FALSE,
//the backend is exited.
peak_bool CIDSPeak::checkForSuccess(peak_status checkStatus, peak_bool continueExecution)
{
    if (PEAK_ERROR(checkStatus))
    {
        peak_status lastErrorCode = PEAK_STATUS_SUCCESS;
        size_t lastErrorMessageSize = 0;

        // Get size of error message
        status = peak_Library_GetLastError(&lastErrorCode, NULL, &lastErrorMessageSize);
        if (PEAK_ERROR(status))
        {
            // Something went wrong getting the last error!
            printf("Last-Error: Getting last error code failed! Status: %#06x\n", status);
            return PEAK_FALSE;
        }

        if (checkStatus != lastErrorCode)
        {
            // Another error occured in the meantime. Proceed with the last error.
            printf("Last-Error: Another error occured in the meantime!\n");
        }

        // Allocate and zero-initialize the char array for the error message
        char* lastErrorMessage = (char*)calloc((lastErrorMessageSize) / sizeof(char), sizeof(char));
        if (lastErrorMessage == NULL)
        {
            // Cannot allocate lastErrorMessage. Most likely not enough Memory.
            printf("Last-Error: Failed to allocate memory for the error message!\n");
            free(lastErrorMessage);
            return PEAK_FALSE;
        }

        // Get the error message
        status = peak_Library_GetLastError(&lastErrorCode, lastErrorMessage, &lastErrorMessageSize);
        if (PEAK_ERROR(status))
        {
            // Unable to get error message. This shouldn't ever happen.
            printf("Last-Error: Getting last error message failed! Status: %#06x; Last error code: %#06x\n", status,
                lastErrorCode);
            free(lastErrorMessage);
            return PEAK_FALSE;
        }

        printf("Last-Error: %s | Code: %#06x\n", lastErrorMessage, lastErrorCode);
        free(lastErrorMessage);

        if (!continueExecution)
        {
            cleanExit();
        }

        return PEAK_FALSE;
    }
    return PEAK_TRUE;
}

int CIDSPeak::getSensorInfo()
{
    // check if the feature is readable
    if (PEAK_IS_READABLE(
        peak_GFA_Feature_GetAccessStatus(hCam, PEAK_GFA_MODULE_REMOTE_DEVICE, "DeviceFirmwareVersion")))
    {
        int64_t temp_x;
        int64_t temp_y;
        status = getGFAInt("WidthMax", &temp_x);
        status = getGFAInt("HeightMax", &temp_y);
        cameraCCDXSize_ = (long)temp_x;
        cameraCCDYSize_ = (long)temp_y;
    }
    else
    {
        return ERR_NO_READ_ACCESS;
    }
    if (status == PEAK_STATUS_SUCCESS) { return DEVICE_OK; }
    else { return DEVICE_ERR; }
}

peak_status CIDSPeak::getGFAString(const char* featureName, char* stringValue)
{
    size_t stringLength = 0;

    // get the length of the feature string
    status = peak_GFA_String_Get(hCam, PEAK_GFA_MODULE_REMOTE_DEVICE, featureName, NULL, &stringLength);

    // if successful, read the firmware version
    if (checkForSuccess(status, PEAK_TRUE))
    {
        // read the string value of featureName
        status = peak_GFA_String_Get(
            hCam, PEAK_GFA_MODULE_REMOTE_DEVICE, featureName, stringValue, &stringLength);
    }
    return status;
}

peak_status CIDSPeak::getGFAInt(const char* featureName, int64_t* intValue)
{
    // read the integer value of featureName
    status = peak_GFA_Integer_Get(
        hCam, PEAK_GFA_MODULE_REMOTE_DEVICE, featureName, intValue);
    return status;
}

peak_status CIDSPeak::getGFAfloat(const char* featureName, double* floatValue)
{
    // read the float value of featureName
    status = peak_GFA_Float_Get(
        hCam, PEAK_GFA_MODULE_REMOTE_DEVICE, featureName, floatValue);
    return status;
}

void CIDSPeak::initializeAutoWBConversion()
{
    peakAutoToString.insert(pair<int, string>(PEAK_AUTO_FEATURE_MODE_OFF, "Off"));
    peakAutoToString.insert(pair<int, string>(PEAK_AUTO_FEATURE_MODE_ONCE, "Once"));
    peakAutoToString.insert(pair<int, string>(PEAK_AUTO_FEATURE_MODE_CONTINUOUS, "Continuous"));

    stringToPeakAuto.insert(pair<string, int>("Off", PEAK_AUTO_FEATURE_MODE_OFF));
    stringToPeakAuto.insert(pair<string, int>("Once", PEAK_AUTO_FEATURE_MODE_ONCE));
    stringToPeakAuto.insert(pair<string, int>("Continuous", PEAK_AUTO_FEATURE_MODE_CONTINUOUS));
}

peak_status CIDSPeak::getPixelTypes(vector<string>& pixelTypeValues)
{
    size_t pixelFormatCount = 0;
    if (peak_PixelFormat_GetAccessStatus(hCam) == PEAK_ACCESS_READWRITE ||
        peak_PixelFormat_GetAccessStatus(hCam) == PEAK_ACCESS_READONLY)
    {
        status = peak_PixelFormat_GetList(hCam, NULL, &pixelFormatCount);
        peak_pixel_format* pixelFormatList = (peak_pixel_format*)calloc(
            pixelFormatCount, sizeof(peak_pixel_format));
        status = peak_PixelFormat_GetList(hCam, pixelFormatList, &pixelFormatCount);

        printf("Available pixel formats: \n");
        for (size_t i = 0; i < pixelFormatCount; i++)
        {
            if (peakTypeToString.find(pixelFormatList[i]) != peakTypeToString.end())
            {
                pixelTypeValues.push_back(peakTypeToString[pixelFormatList[i]].c_str());
            }
        }
    }
    return status;
}

int CIDSPeak::transferBuffer(peak_frame_handle hFrame, ImgBuffer& img)
{
    peak_frame_handle hFrameConverted;
    peak_buffer peakBuffer;
    uint8_t* memoryAddress;
    size_t memorySize;
    unsigned char* pBuf = (unsigned char*) const_cast<unsigned char*>(img.GetPixels());

    // Convert data types to MM supported data types
    // Monochrome is natively supported by MM, so no conversion is needed
    if (nComponents_ == 1)
    {
        status = peak_Frame_Buffer_Get(hFrame, &peakBuffer);
        // Transfer the frame buffer to the img buffer expected by MM.
        memoryAddress = peakBuffer.memoryAddress;
        memorySize = peakBuffer.memorySize;
        memcpy(pBuf, memoryAddress, memorySize);
    }
    // Convert all 8bit pixel formats into BGRA8 (8bit format expected by MM)
    else if (nComponents_ == 4)
    {
        status = peak_IPL_PixelFormat_Set(hCam, PEAK_PIXEL_FORMAT_BGRA8);
        if (status != PEAK_STATUS_SUCCESS) { return DEVICE_UNSUPPORTED_DATA_FORMAT; }
        status = peak_IPL_ProcessFrame(hCam, hFrame, &hFrameConverted);
        if (status != PEAK_STATUS_SUCCESS) { return DEVICE_UNSUPPORTED_DATA_FORMAT; }
        status = peak_Frame_Buffer_Get(hFrameConverted, &peakBuffer);
        // Transfer the frame buffer to the img buffer expected by MM.
        memoryAddress = peakBuffer.memoryAddress;
        memorySize = peakBuffer.memorySize;
        memcpy(pBuf, memoryAddress, memorySize);
        peak_Frame_Release(hCam, hFrameConverted);
    }
    else
    {
        return DEVICE_UNSUPPORTED_DATA_FORMAT;
    }

    // Exit if something went wrong during the conversion/obtaining the buffer.
    if (status != PEAK_STATUS_SUCCESS) { return DEVICE_UNSUPPORTED_DATA_FORMAT; }

    return DEVICE_OK;
}

int CIDSPeak::updateAutoWhiteBalance()
{
    if (peak_AutoWhiteBalance_GetAccessStatus(hCam) == PEAK_ACCESS_READONLY
        || peak_AutoWhiteBalance_GetAccessStatus(hCam) == PEAK_ACCESS_READWRITE)
    {
        // Update the gain channels
        status = peak_Gain_Get(hCam, PEAK_GAIN_TYPE_DIGITAL, PEAK_GAIN_CHANNEL_MASTER, &gainMaster_);
        status = peak_Gain_Get(hCam, PEAK_GAIN_TYPE_DIGITAL, PEAK_GAIN_CHANNEL_RED, &gainRed_);
        status = peak_Gain_Get(hCam, PEAK_GAIN_TYPE_DIGITAL, PEAK_GAIN_CHANNEL_GREEN, &gainGreen_);
        status = peak_Gain_Get(hCam, PEAK_GAIN_TYPE_DIGITAL, PEAK_GAIN_CHANNEL_BLUE, &gainBlue_);
        // Update the auto white balance mode
        status = peak_AutoWhiteBalance_Mode_Get(hCam, &peakAutoWhiteBalance_);
    }
    else { return ERR_NO_READ_ACCESS; }

    if (status == PEAK_STATUS_SUCCESS) { return DEVICE_OK; }
    else { return DEVICE_ERR; }
}

int CIDSPeak::framerateSet(double interval_ms)
{
    int nRet = DEVICE_OK;
    // Make sure interval is less than exposure time
    // Half a millisecond buffer to make sure sensor can dump info
    if (interval_ms < exposureCur_ + 0.5)
    {
        interval_ms = exposureCur_ + 0.5;
    }

    // Check if interval doesn't exceed framerate limitations of camera
    // Else set interval to match max framerate
    if (1000 / interval_ms > framerateMax_)
    {
        interval_ms = 1000 / framerateMax_;
    }

    status = peak_FrameRate_Set(hCam, 1000 / interval_ms);
    framerateCur_ = 1000 / interval_ms;
    if (status != DEVICE_OK)
    {
        return ERR_NO_WRITE_ACCESS;
    }
    return nRet;
}