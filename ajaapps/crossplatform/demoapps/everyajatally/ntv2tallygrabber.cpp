/* SPDX-License-Identifier: MIT */
/**
    @file        ntv2tallygrabber.cpp
    @brief        implements the NTV2TallyGrabber class.
    @copyright    (C) 2023-2022 Everycast labs All rights reserved.
**/
/* devived from
	@file		ntv2capture.cpp
	@brief		Implementation of NTV2Capture class.
	@copyright	(C) 2012-2022 AJA Video Systems, Inc.  All rights reserved.
**/

#include "ntv2tallygrabber.h"
#include "ntv2utils.h"
#include "ntv2debug.h"	//	for NTV2DeviceString
#include "ntv2devicefeatures.h"
#include "ajabase/system/process.h"
#include "ajabase/system/systemtime.h"
#include "ajaanc/includes/ancillarylist.h"
#include <iterator>	//	for inserter
#include <fstream>	//	for ofstream

using namespace std;



//////////////////////////////////////////////////////////////////////////////////////	NTV2Capture IMPLEMENTATION

NTV2TallyGrabber::NTV2TallyGrabber (const CaptureConfig & inConfig)
	:	mConsumerThread		(AJAThread()),
		mProducerThread		(AJAThread()),
		mDeviceID			(DEVICE_ID_NOTFOUND),
		mConfig				(inConfig),
		mVideoFormat		(NTV2_FORMAT_UNKNOWN),
		mSavedTaskMode		(NTV2_DISABLE_TASKS),
		mAudioSystem		(NTV2_AUDIOSYSTEM_INVALID),
		mGlobalQuit			(false),
		mHostBuffers		(),
		mAVCircularBuffer	()
{
}	//	constructor


NTV2TallyGrabber::~NTV2TallyGrabber ()
{
	//	Stop my capture and consumer threads, then destroy them...
	Quit();

	//	Unsubscribe from input vertical event...
	mDevice.UnsubscribeInputVerticalEvent(mConfig.fInputChannel);

}	//	destructor


void NTV2TallyGrabber::Quit (void)
{
	//	Set the global 'quit' flag, and wait for the threads to go inactive...
	mGlobalQuit = true;

	while (mConsumerThread.Active())
		AJATime::Sleep(10);

	while (mProducerThread.Active())
		AJATime::Sleep(10);

	if (!mConfig.fDoMultiFormat)
	{
		mDevice.ReleaseStreamForApplication (kDemoAppSignature, static_cast<int32_t>(AJAProcess::GetPid()));
		mDevice.SetEveryFrameServices(mSavedTaskMode);		//	Restore prior task mode
	}

}	//	Quit


AJAStatus NTV2TallyGrabber::Init (void)
{
	AJAStatus	status	(AJA_STATUS_SUCCESS);

	//	Open the device...
	if (!CNTV2DeviceScanner::GetFirstDeviceFromArgument (mConfig.fDeviceSpec, mDevice))
		{cerr << "## ERROR:  Device '" << mConfig.fDeviceSpec << "' not found" << endl;  return AJA_STATUS_OPEN;}

    if (!mDevice.IsDeviceReady(false))
		{cerr << "## ERROR:  Device '" << mConfig.fDeviceSpec << "' not ready" << endl;  return AJA_STATUS_INITIALIZE;}

	mDeviceID = mDevice.GetDeviceID();						//	Keep the device ID handy, as it's used frequently
	if (!::NTV2DeviceCanDoCapture(mDeviceID))
		{cerr << "## ERROR:  Device '" << mConfig.fDeviceSpec << "' cannot capture" << endl;  return AJA_STATUS_FEATURE;}

	ULWord	appSignature	(0);
	int32_t	appPID			(0);
	mDevice.GetStreamingApplication (appSignature, appPID);	//	Who currently "owns" the device?
	mDevice.GetEveryFrameServices(mSavedTaskMode);			//	Save the current device state
	if (!mConfig.fDoMultiFormat)
	{
		if (!mDevice.AcquireStreamForApplication (kDemoAppSignature, static_cast<int32_t>(AJAProcess::GetPid())))
		{
			cerr << "## ERROR:  Unable to acquire device because another app (pid " << appPID << ") owns it" << endl;
			return AJA_STATUS_BUSY;		//	Another app is using the device
		}
		mDevice.GetEveryFrameServices(mSavedTaskMode);		//	Save the current state before we change it
	}
	mDevice.SetEveryFrameServices(NTV2_OEM_TASKS);			//	Since this is an OEM demo, use the OEM service level

	if (::NTV2DeviceCanDoMultiFormat(mDeviceID))
		mDevice.SetMultiFormatMode(mConfig.fDoMultiFormat);

	if (!NTV2_IS_VALID_CHANNEL(mConfig.fInputChannel)  &&  !NTV2_IS_VALID_INPUT_SOURCE(mConfig.fInputSource))
		mConfig.fInputChannel = NTV2_CHANNEL1;
	else if (!NTV2_IS_VALID_CHANNEL(mConfig.fInputChannel)  &&  NTV2_IS_VALID_INPUT_SOURCE(mConfig.fInputSource))
		mConfig.fInputChannel = ::NTV2InputSourceToChannel(mConfig.fInputSource);
	else if (NTV2_IS_VALID_CHANNEL(mConfig.fInputChannel)  &&  !NTV2_IS_VALID_INPUT_SOURCE(mConfig.fInputSource))
		mConfig.fInputSource = ::NTV2ChannelToInputSource(mConfig.fInputChannel, NTV2_INPUTSOURCES_SDI);
	//	On KonaHDMI, map specified SDI input to equivalent HDMI input...
	if (::NTV2DeviceGetNumHDMIVideoInputs(mDeviceID) > 1  &&  NTV2_INPUT_SOURCE_IS_SDI(mConfig.fInputSource))
		mConfig.fInputSource = ::NTV2ChannelToInputSource(::NTV2InputSourceToChannel(mConfig.fInputSource), NTV2_INPUTSOURCES_HDMI);

	//	Set up the video
	status = SetupVideo();
	if (AJA_FAILURE(status))
		return status;

	//	Set up the circular buffers, the device signal routing, and both playout and capture AutoCirculate...
	SetupHostBuffers();
	RouteInputSignal();

	#if defined(_DEBUG)
		cerr << mConfig << endl;
	#endif	//	defined(_DEBUG)
	return AJA_STATUS_SUCCESS;

}	//	Init


AJAStatus NTV2TallyGrabber::SetupVideo (void)
{
	//	Sometimes other applications disable some or all of the frame buffers, so turn on ours now...
	mDevice.EnableChannel(mConfig.fInputChannel);

	//	Enable and subscribe to the interrupts for the channel to be used...
	mDevice.EnableInputInterrupt(mConfig.fInputChannel);
	mDevice.SubscribeInputVerticalEvent(mConfig.fInputChannel);
	mDevice.SubscribeOutputVerticalEvent(NTV2_CHANNEL1);

	//	If the device supports bi-directional SDI and the
	//	requested input is SDI, ensure the SDI connector
	//	is configured to receive...
	if (::NTV2DeviceHasBiDirectionalSDI(mDeviceID) && NTV2_INPUT_SOURCE_IS_SDI(mConfig.fInputSource))
	{
		mDevice.SetSDITransmitEnable(mConfig.fInputChannel, false);

		//	Give the input circuit some time (~10 VBIs) to lock onto the input signal...
		mDevice.WaitForInputVerticalInterrupt (mConfig.fInputChannel, 10);
	}

	//	Determine the input video signal format...
	mVideoFormat = mDevice.GetInputVideoFormat (mConfig.fInputSource);
	if (mVideoFormat == NTV2_FORMAT_UNKNOWN)
	{
		cerr << "## ERROR:  No input signal or unknown format" << endl;
		return AJA_STATUS_NOINPUT;	//	Sorry, can't handle this format
	}

	//	Set the device video format to whatever we detected at the input...
	mDevice.SetReference (::NTV2InputSourceToReferenceSource(mConfig.fInputSource));
	mDevice.SetVideoFormat (mVideoFormat, false, false, mConfig.fInputChannel);

	//	Set the frame buffer pixel format for all the channels on the device
	//	(assuming it supports that pixel format -- otherwise default to 8-bit YCbCr)...
	if (!::NTV2DeviceCanDoFrameBufferFormat (mDeviceID, mConfig.fPixelFormat))
	{
		cerr	<< "## WARNING:  " << ::NTV2FrameBufferFormatToString(mConfig.fPixelFormat)
				<< " unsupported, using " << ::NTV2FrameBufferFormatToString(NTV2_FBF_8BIT_YCBCR)
				<< " instead" << endl;
		mConfig.fPixelFormat = NTV2_FBF_8BIT_YCBCR;
	}
	mDevice.SetFrameBufferFormat (mConfig.fInputChannel, mConfig.fPixelFormat);

	//	Disable Anc capture if the device can't do it...
	if (!::NTV2DeviceCanDoCustomAnc (mDeviceID))
        return AJA_STATUS_FAIL;

	return AJA_STATUS_SUCCESS;

}	//	SetupVideo


void NTV2TallyGrabber::SetupHostBuffers (void)
{
	NTV2VANCMode	vancMode(NTV2_VANCMODE_INVALID);
	NTV2Standard	standard(NTV2_STANDARD_INVALID);
	ULWord			F1AncSize(0), F2AncSize(0);
	mDevice.GetVANCMode (vancMode);
	mDevice.GetStandard (standard);

	//	Let my circular buffer know when it's time to quit...
	mAVCircularBuffer.SetAbortFlag (&mGlobalQuit);


    ULWord	F1OffsetFromEnd(0), F2OffsetFromEnd(0);
    mDevice.ReadRegister(kVRegAncField1Offset, F1OffsetFromEnd);	//	# bytes from end of 8MB/16MB frame
    mDevice.ReadRegister(kVRegAncField2Offset, F2OffsetFromEnd);	//	# bytes from end of 8MB/16MB frame
    //	Based on the offsets, calculate the max anc capacity
    F1AncSize = F2OffsetFromEnd > F1OffsetFromEnd ? 0 : F1OffsetFromEnd - F2OffsetFromEnd;
    F2AncSize = F2OffsetFromEnd > F1OffsetFromEnd ? F2OffsetFromEnd - F1OffsetFromEnd : F2OffsetFromEnd;

	mFormatDesc = NTV2FormatDescriptor (standard, mConfig.fPixelFormat, vancMode);

	//	Allocate and add each in-host NTV2FrameData to my circular buffer member variable...
	mHostBuffers.reserve(size_t(CIRCULAR_BUFFER_SIZE));
	while (mHostBuffers.size() < size_t(CIRCULAR_BUFFER_SIZE))
	{
		mHostBuffers.push_back(NTV2FrameData());
		NTV2FrameData & frameData(mHostBuffers.back());
		frameData.fVideoBuffer.Allocate(::GetVideoWriteSize (mVideoFormat, mConfig.fPixelFormat, vancMode));
		frameData.fAudioBuffer.Allocate(0);
		frameData.fAncBuffer.Allocate(F1AncSize);
		frameData.fAncBuffer2.Allocate(F2AncSize);
		mAVCircularBuffer.Add(&frameData);
	}	//	for each NTV2FrameData

}	//	SetupHostBuffers


void NTV2TallyGrabber::RouteInputSignal (void)
{
	//	For this simple example, tie the user-selected input to frame buffer 1.
	//	Is this user-selected input supported on the device?
	if (!::NTV2DeviceCanDoInputSource (mDeviceID, mConfig.fInputSource))
		mConfig.fInputSource = NTV2_INPUTSOURCE_SDI1;

	NTV2LHIHDMIColorSpace	inputColor	(NTV2_LHIHDMIColorSpaceYCbCr);
	if (NTV2_INPUT_SOURCE_IS_HDMI(mConfig.fInputSource))
		mDevice.GetHDMIInputColor (inputColor, mConfig.fInputChannel);

	const bool						isInputRGB				(inputColor == NTV2_LHIHDMIColorSpaceRGB);
	const bool						isFrameRGB				(::IsRGBFormat(mConfig.fPixelFormat));
	const NTV2OutputCrosspointID	inputWidgetOutputXpt	(::GetInputSourceOutputXpt(mConfig.fInputSource, false, isInputRGB, 0));
	const NTV2InputCrosspointID		frameBufferInputXpt		(::GetFrameBufferInputXptFromChannel(mConfig.fInputChannel));
	const NTV2InputCrosspointID		cscWidgetVideoInputXpt	(::GetCSCInputXptFromChannel(mConfig.fInputChannel));
	const NTV2OutputCrosspointID	cscWidgetRGBOutputXpt	(::GetCSCOutputXptFromChannel(mConfig.fInputChannel, /*inIsKey*/ false, /*inIsRGB*/ true));
	const NTV2OutputCrosspointID	cscWidgetYUVOutputXpt	(::GetCSCOutputXptFromChannel(mConfig.fInputChannel, /*inIsKey*/ false, /*inIsRGB*/ false));


	if (!mConfig.fDoMultiFormat)
		mDevice.ClearRouting ();

	if (isInputRGB && !isFrameRGB)
	{
		mDevice.Connect (frameBufferInputXpt,		cscWidgetYUVOutputXpt);	//	Frame store input to CSC widget's YUV output
		mDevice.Connect (cscWidgetVideoInputXpt,	inputWidgetOutputXpt);	//	CSC widget's RGB input to input widget's output
	}
	else if (!isInputRGB && isFrameRGB)
	{
		mDevice.Connect (frameBufferInputXpt,		cscWidgetRGBOutputXpt);	//	Frame store input to CSC widget's RGB output
		mDevice.Connect (cscWidgetVideoInputXpt,	inputWidgetOutputXpt);	//	CSC widget's YUV input to input widget's output
	}
	else
		mDevice.Connect (frameBufferInputXpt,		inputWidgetOutputXpt);	//	Frame store input to input widget's output

}	//	RouteInputSignal


AJAStatus NTV2TallyGrabber::Run ()
{
	//	Start the playout and capture threads...
	StartConsumerThread ();
	StartProducerThread ();
	return AJA_STATUS_SUCCESS;

}	//	Run


//////////////////////////////////////////////////////////////////////////////////////////////////////////

//	This is where we will start the consumer thread
void NTV2TallyGrabber::StartConsumerThread (void)
{
	//	Create and start the consumer thread...
	mConsumerThread.Attach(ConsumerThreadStatic, this);
	mConsumerThread.SetPriority(AJA_ThreadPriority_High);
	mConsumerThread.Start();

}	//	StartConsumerThread


//	The consumer thread function
void NTV2TallyGrabber::ConsumerThreadStatic (AJAThread * pThread, void * pContext)		//	static
{
	(void) pThread;

	//	Grab the NTV2Capture instance pointer from the pContext parameter,
	//	then call its ConsumeFrames method...
	NTV2TallyGrabber *	pApp	(reinterpret_cast <NTV2TallyGrabber *> (pContext));
	pApp->ConsumeFrames ();

}	//	ConsumerThreadStatic


void NTV2TallyGrabber::ConsumeFrames (void)
{
	CAPNOTE("Thread started");
	uint32_t ancTally(0);
	while (!mGlobalQuit)
	{
		//	Wait for the next frame to become ready to "consume"...
		NTV2FrameData *	pFrameData	(mAVCircularBuffer.StartConsumeNextBuffer ());
		if (pFrameData)
		{
            AJAAncillaryList    ancPackets, vancPackets;


            const NTV2_POINTER validAncF1 (pFrameData->AncBuffer(), streamsize(pFrameData->AncBufferSize()));
            const NTV2_POINTER validAncF2 (pFrameData->AncBuffer2(), streamsize(pFrameData->AncBuffer2Size()));
            AJAAncillaryList::SetFromDeviceAncBuffers (validAncF1, validAncF2, ancPackets, ancTally);
            ancPackets.ParseAllAncillaryData();


            int npac = ancPackets.CountAncillaryData();
            for (uint32_t p = 0; p< npac; p++){
                AJAAncillaryData *ap = ancPackets.GetAncillaryDataAtIndex(p);
                //cerr << "------- ancillary data packet " << p << " frame " << ancTally << endl;
                //ap->DumpPayload(cout);
                uint8_t did = ap->GetDID();
                uint8_t sid = ap->GetSID();
                int l = ap->GetLocationLineNumber();
                if ((l == 15) && (did == 0x51) && (sid == 0x52)) {
                    AJAAncillaryDataType t = ap -> GetAncillaryDataType();
                    cerr << "GOOD did = " << (int) did << " sid = "<< (int) sid << " line " << l << " type "<< (int) (t) << endl;
                    size_t c = ap->GetPayloadByteCount();
                    const uint8_t * data = ap->GetPayloadData();
                    int source = 1;
                    for (int i=1; i<c; i++) {
                        uint8_t v = *(data +i);
                        cerr << "source "<< source << "t=" << (int) (v&3) << endl;
                        source++;
                        cerr << "source "<< source << "t=" << (int) ((v>>4)&3) << endl;
                        source++;
                    }
                } /*else {
                    cerr << "BAD did = " << (int) did<< " sid = "<< (int) sid << endl;
                }*/
            }

			//	Now release and recycle the buffer...
			mAVCircularBuffer.EndConsumeNextBuffer ();
		}	//	if pFrameData
        ancTally++;
	}	//	loop til quit signaled
	CAPNOTE("Thread completed, will exit");

}	//	ConsumeFrames


//////////////////////////////////////////////////////////////////////////////////////////////////////////

//	This is where we start the capture thread
void NTV2TallyGrabber::StartProducerThread (void)
{
	//	Create and start the capture thread...
	mProducerThread.Attach(ProducerThreadStatic, this);
	mProducerThread.SetPriority(AJA_ThreadPriority_High);
	mProducerThread.Start();

}	//	StartProducerThread


//	The capture thread function
void NTV2TallyGrabber::ProducerThreadStatic (AJAThread * pThread, void * pContext)		//	static
{
	(void) pThread;

	//	Grab the NTV2Capture instance pointer from the pContext parameter,
	//	then call its CaptureFrames method...
	NTV2TallyGrabber *	pApp	(reinterpret_cast <NTV2TallyGrabber *> (pContext));
	pApp->CaptureFrames ();

}	//	ProducerThreadStatic


void NTV2TallyGrabber::CaptureFrames (void)
{
	AUTOCIRCULATE_TRANSFER	inputXfer;	//	My A/C input transfer info
	ULWord					acOptions (AUTOCIRCULATE_WITH_RP188), overruns(0);
	UWord					sdiSpigot (UWord(::NTV2InputSourceToChannel(mConfig.fInputSource)));

    acOptions |= AUTOCIRCULATE_WITH_ANC;

	CAPNOTE("Thread started");
	//	Initialize and start capture AutoCirculate...
	mDevice.AutoCirculateStop(mConfig.fInputChannel);	//	Just in case
	mDevice.AutoCirculateInitForInput (	mConfig.fInputChannel,		//	primary channel
										mConfig.fFrames.count(),	//	numFrames (zero if specifying range)
										mAudioSystem,				//	audio system
										acOptions,					//	flags
										1,							//	numChannels to gang
										mConfig.fFrames.firstFrame(), mConfig.fFrames.lastFrame());
	mDevice.AutoCirculateStart(mConfig.fInputChannel);

	while (!mGlobalQuit)
	{
		AUTOCIRCULATE_STATUS acStatus;
		mDevice.AutoCirculateGetStatus (mConfig.fInputChannel, acStatus);

		if (acStatus.IsRunning()  &&  acStatus.HasAvailableInputFrame())
		{
			//	At this point, there's at least one fully-formed frame available in the device's
			//	frame buffer to transfer to the host. Reserve an NTV2FrameData to "produce", and
			//	use it in the next transfer from the device...
			NTV2FrameData *	pCaptureData(mAVCircularBuffer.StartProduceNextBuffer());

			inputXfer.SetVideoBuffer (pCaptureData->VideoBuffer(), pCaptureData->VideoBufferSize());
			if (acStatus.WithCustomAnc())
				inputXfer.SetAncBuffers (pCaptureData->AncBuffer(), pCaptureData->AncBufferSize(),
										 pCaptureData->AncBuffer2(), pCaptureData->AncBuffer2Size());

			//	Transfer video/audio/anc from the device into our host buffers...
			mDevice.AutoCirculateTransfer (mConfig.fInputChannel, inputXfer);

			//	If capturing Anc, clear stale anc data from the anc buffers...
			if (acStatus.WithCustomAnc()  &&  pCaptureData->AncBuffer())
			{	bool overrun(false);
				mDevice.AncExtractGetBufferOverrun (sdiSpigot, overrun);
				if (overrun)
					{overruns++;  CAPWARN(overruns << " anc overrun(s)");}
				pCaptureData->fNumAncBytes = inputXfer.GetCapturedAncByteCount(/*isF2*/false);
				NTV2_POINTER stale (pCaptureData->fAncBuffer.GetHostAddress(pCaptureData->fNumAncBytes),
									pCaptureData->fAncBuffer.GetByteCount() - pCaptureData->fNumAncBytes);
				stale.Fill(uint8_t(0));
			}
			if (acStatus.WithCustomAnc()  &&  pCaptureData->AncBuffer2())
			{
				pCaptureData->fNumAnc2Bytes = inputXfer.GetCapturedAncByteCount(/*isF2*/true);
				NTV2_POINTER stale (pCaptureData->fAncBuffer2.GetHostAddress(pCaptureData->fNumAnc2Bytes),
									pCaptureData->fAncBuffer2.GetByteCount() - pCaptureData->fNumAnc2Bytes);
				stale.Fill(uint8_t(0));
			}

			//	Grab all valid timecodes that were captured...
			inputXfer.GetInputTimeCodes (pCaptureData->fTimecodes, mConfig.fInputChannel, /*ValidOnly*/ true);


			//	Signal that we're done "producing" the frame, making it available for future "consumption"...
			mAVCircularBuffer.EndProduceNextBuffer();
		}	//	if A/C running and frame(s) are available for transfer
		else
		{
			//	Either AutoCirculate is not running, or there were no frames available on the device to transfer.
			//	Rather than waste CPU cycles spinning, waiting until a frame becomes available, it's far more
			//	efficient to wait for the next input vertical interrupt event to get signaled...
			mDevice.WaitForInputVerticalInterrupt(mConfig.fInputChannel);
		}

		//	Log SDI input CRC/VPID/TRS errors...
		if (::NTV2DeviceCanDoSDIErrorChecks(mDeviceID) && NTV2_INPUT_SOURCE_IS_SDI(mConfig.fInputSource))
		{
			NTV2SDIInStatistics	sdiStats;
			NTV2SDIInputStatus	inputStatus;
			if (mDevice.ReadSDIStatistics(sdiStats))
			{
				sdiStats.GetSDIInputStatus(inputStatus, UWord(::GetIndexForNTV2InputSource(mConfig.fInputSource)));
				if (!inputStatus.mLocked)
					CAPWARN(inputStatus);
			}
		}
	}	//	loop til quit signaled

	//	Stop AutoCirculate...
	mDevice.AutoCirculateStop(mConfig.fInputChannel);
	CAPNOTE("Thread completed, will exit");

}	//	CaptureFrames


void NTV2TallyGrabber::GetACStatus (ULWord & outGoodFrames, ULWord & outDroppedFrames, ULWord & outBufferLevel)
{
	AUTOCIRCULATE_STATUS	status;
	mDevice.AutoCirculateGetStatus (mConfig.fInputChannel, status);
	outGoodFrames = status.acFramesProcessed;
	outDroppedFrames = status.acFramesDropped;
	outBufferLevel = status.acBufferLevel;
}


//////////////////////////////////////////////


AJALabelValuePairs CaptureConfig::Get (const bool inCompact) const
{
	AJALabelValuePairs result;
	AJASystemInfo::append(result, "Capture Config");
	AJASystemInfo::append(result, "Device Specifier",	fDeviceSpec);
	AJASystemInfo::append(result, "Input Channel",		::NTV2ChannelToString(fInputChannel, inCompact));
	AJASystemInfo::append(result, "Input Source",		::NTV2InputSourceToString(fInputSource, inCompact));
	AJASystemInfo::append(result, "Pixel Format",		::NTV2FrameBufferFormatToString(fPixelFormat, inCompact));
	AJASystemInfo::append(result, "AutoCirc Frames",	fFrames.toString());
	AJASystemInfo::append(result, "MultiFormat Mode",	fDoMultiFormat ? "Y" : "N");
	return result;
}


std::ostream & operator << (std::ostream & ioStrm,  const CaptureConfig & inObj)
{
	ioStrm	<< AJASystemInfo::ToString(inObj.Get());
	return ioStrm;
}
