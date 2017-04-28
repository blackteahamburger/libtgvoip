//
// libtgvoip is free and unencumbered public domain software.
// For more information, see http://unlicense.org or the UNLICENSE file
// you should have received with this source code distribution.
//

#include <stdlib.h>
#include <stdio.h>
#include "AudioOutputAudioUnitOSX.h"
#include "../../logging.h"
#include "../../VoIPController.h"

#define BUFFER_SIZE 960
#define CHECK_AU_ERROR(res, msg) if(res!=noErr){ LOGE("output: " msg": OSStatus=%d", (int)res); return; }

#define kOutputBus 0
#define kInputBus 1

using namespace tgvoip;
using namespace tgvoip::audio;

AudioOutputAudioUnit::AudioOutputAudioUnit(std::string deviceID){
	remainingDataSize=0;
	isPlaying=false;
	
	OSStatus status;
	AudioComponentDescription inputDesc={
		.componentType = kAudioUnitType_Output, .componentSubType = kAudioUnitSubType_HALOutput, .componentFlags = 0, .componentFlagsMask = 0,
		.componentManufacturer = kAudioUnitManufacturer_Apple
	};
	AudioComponent component=AudioComponentFindNext(NULL, &inputDesc);
	status=AudioComponentInstanceNew(component, &unit);
	CHECK_AU_ERROR(status, "Error creating AudioUnit");
	
	UInt32 flag=1;
	status = AudioUnitSetProperty(unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, kOutputBus, &flag, sizeof(flag));
	CHECK_AU_ERROR(status, "Error enabling AudioUnit output");
	flag=0;
	status = AudioUnitSetProperty(unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, kInputBus, &flag, sizeof(flag));
	CHECK_AU_ERROR(status, "Error enabling AudioUnit input");
	
	SetCurrentDevice(deviceID);
	
	CFRunLoopRef theRunLoop = NULL;
	AudioObjectPropertyAddress propertyAddress = { kAudioHardwarePropertyRunLoop,
		kAudioObjectPropertyScopeGlobal,
		kAudioObjectPropertyElementMaster };
	status = AudioObjectSetPropertyData(kAudioObjectSystemObject, &propertyAddress, 0, NULL, sizeof(CFRunLoopRef), &theRunLoop);
	
	propertyAddress.mSelector = kAudioHardwarePropertyDefaultOutputDevice;
	propertyAddress.mScope = kAudioObjectPropertyScopeGlobal;
	propertyAddress.mElement = kAudioObjectPropertyElementMaster;
	AudioObjectAddPropertyListener(kAudioObjectSystemObject, &propertyAddress, AudioOutputAudioUnit::DefaultDeviceChangedCallback, this);
	
	AudioStreamBasicDescription desiredFormat={
		.mSampleRate=/*hardwareFormat.mSampleRate*/48000, .mFormatID=kAudioFormatLinearPCM, .mFormatFlags=kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked | kAudioFormatFlagsNativeEndian,
		.mFramesPerPacket=1, .mChannelsPerFrame=1, .mBitsPerChannel=16, .mBytesPerPacket=2, .mBytesPerFrame=2
	};
	
	status=AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, kOutputBus, &desiredFormat, sizeof(desiredFormat));
	CHECK_AU_ERROR(status, "Error setting format");
	
	AURenderCallbackStruct callbackStruct;
	callbackStruct.inputProc = AudioOutputAudioUnit::BufferCallback;
	callbackStruct.inputProcRefCon=this;
	status = AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Global, kOutputBus, &callbackStruct, sizeof(callbackStruct));
	CHECK_AU_ERROR(status, "Error setting input buffer callback");
	status=AudioUnitInitialize(unit);
	CHECK_AU_ERROR(status, "Error initializing unit");
}

AudioOutputAudioUnit::~AudioOutputAudioUnit(){
	AudioObjectPropertyAddress propertyAddress;
	propertyAddress.mSelector = kAudioHardwarePropertyDefaultOutputDevice;
	propertyAddress.mScope = kAudioObjectPropertyScopeGlobal;
	propertyAddress.mElement = kAudioObjectPropertyElementMaster;
	AudioObjectRemovePropertyListener(kAudioObjectSystemObject, &propertyAddress, AudioOutputAudioUnit::DefaultDeviceChangedCallback, this);
	
	AudioUnitUninitialize(unit);
	AudioComponentInstanceDispose(unit);
}

void AudioOutputAudioUnit::Configure(uint32_t sampleRate, uint32_t bitsPerSample, uint32_t channels){
}

void AudioOutputAudioUnit::Start(){
	isPlaying=true;
	OSStatus status=AudioOutputUnitStart(unit);
	CHECK_AU_ERROR(status, "Error starting AudioUnit");
}

void AudioOutputAudioUnit::Stop(){
	isPlaying=false;
	OSStatus status=AudioOutputUnitStart(unit);
	CHECK_AU_ERROR(status, "Error stopping AudioUnit");
}

OSStatus AudioOutputAudioUnit::BufferCallback(void *inRefCon, AudioUnitRenderActionFlags *ioActionFlags, const AudioTimeStamp *inTimeStamp, UInt32 inBusNumber, UInt32 inNumberFrames, AudioBufferList *ioData){
	AudioOutputAudioUnit* input=(AudioOutputAudioUnit*) inRefCon;
	input->HandleBufferCallback(ioData);
	return noErr;
}

bool AudioOutputAudioUnit::IsPlaying(){
	return isPlaying;
}

void AudioOutputAudioUnit::HandleBufferCallback(AudioBufferList *ioData){
	int i;
	unsigned int k;
	int16_t absVal=0;
	for(i=0;i<ioData->mNumberBuffers;i++){
		AudioBuffer buf=ioData->mBuffers[i];
		if(!isPlaying){
			memset(buf.mData, 0, buf.mDataByteSize);
			return;
		}
		while(remainingDataSize<buf.mDataByteSize){
			assert(remainingDataSize+BUFFER_SIZE*2<10240);
			InvokeCallback(remainingData+remainingDataSize, BUFFER_SIZE*2);
			remainingDataSize+=BUFFER_SIZE*2;
		}
		memcpy(buf.mData, remainingData, buf.mDataByteSize);
		remainingDataSize-=buf.mDataByteSize;
		memmove(remainingData, remainingData+buf.mDataByteSize, remainingDataSize);
	}
}


void AudioOutputAudioUnit::EnumerateDevices(std::vector<AudioOutputDevice>& devs){
	AudioObjectPropertyAddress propertyAddress = {
		kAudioHardwarePropertyDevices,
		kAudioObjectPropertyScopeGlobal,
		kAudioObjectPropertyElementMaster
	};
	
	UInt32 dataSize = 0;
	OSStatus status = AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &propertyAddress, 0, NULL, &dataSize);
	if(kAudioHardwareNoError != status) {
		LOGE("AudioObjectGetPropertyDataSize (kAudioHardwarePropertyDevices) failed: %i", status);
		return;
	}
	
	UInt32 deviceCount = (UInt32)(dataSize / sizeof(AudioDeviceID));
	
	
	AudioDeviceID *audioDevices = (AudioDeviceID*)(malloc(dataSize));
	
	status = AudioObjectGetPropertyData(kAudioObjectSystemObject, &propertyAddress, 0, NULL, &dataSize, audioDevices);
	if(kAudioHardwareNoError != status) {
		LOGE("AudioObjectGetPropertyData (kAudioHardwarePropertyDevices) failed: %i", status);
		free(audioDevices);
		audioDevices = NULL;
		return;
	}
	
	
	// Iterate through all the devices and determine which are input-capable
	propertyAddress.mScope = kAudioDevicePropertyScopeOutput;
	for(UInt32 i = 0; i < deviceCount; ++i) {
		// Query device UID
		CFStringRef deviceUID = NULL;
		dataSize = sizeof(deviceUID);
		propertyAddress.mSelector = kAudioDevicePropertyDeviceUID;
		status = AudioObjectGetPropertyData(audioDevices[i], &propertyAddress, 0, NULL, &dataSize, &deviceUID);
		if(kAudioHardwareNoError != status) {
			LOGE("AudioObjectGetPropertyData (kAudioDevicePropertyDeviceUID) failed: %i", status);
			continue;
		}
		
		// Query device name
		CFStringRef deviceName = NULL;
		dataSize = sizeof(deviceName);
		propertyAddress.mSelector = kAudioDevicePropertyDeviceNameCFString;
		status = AudioObjectGetPropertyData(audioDevices[i], &propertyAddress, 0, NULL, &dataSize, &deviceName);
		if(kAudioHardwareNoError != status) {
			LOGE("AudioObjectGetPropertyData (kAudioDevicePropertyDeviceNameCFString) failed: %i", status);
			continue;
		}
		
		// Determine if the device is an input device (it is an input device if it has input channels)
		dataSize = 0;
		propertyAddress.mSelector = kAudioDevicePropertyStreamConfiguration;
		status = AudioObjectGetPropertyDataSize(audioDevices[i], &propertyAddress, 0, NULL, &dataSize);
		if(kAudioHardwareNoError != status) {
			LOGE("AudioObjectGetPropertyDataSize (kAudioDevicePropertyStreamConfiguration) failed: %i", status);
			continue;
		}
		
		AudioBufferList *bufferList = (AudioBufferList*)(malloc(dataSize));
		
		status = AudioObjectGetPropertyData(audioDevices[i], &propertyAddress, 0, NULL, &dataSize, bufferList);
		if(kAudioHardwareNoError != status || 0 == bufferList->mNumberBuffers) {
			if(kAudioHardwareNoError != status)
				LOGE("AudioObjectGetPropertyData (kAudioDevicePropertyStreamConfiguration) failed: %i", status);
			free(bufferList);
			bufferList = NULL;
			continue;
		}
		
		free(bufferList);
		bufferList = NULL;
		
		AudioOutputDevice dev;
		char buf[1024];
		CFStringGetCString(deviceName, buf, 1024, kCFStringEncodingUTF8);
		dev.displayName=std::string(buf);
		CFStringGetCString(deviceUID, buf, 1024, kCFStringEncodingUTF8);
		dev.id=std::string(buf);
		devs.push_back(dev);
	}
	
	free(audioDevices);
	audioDevices = NULL;
}

void AudioOutputAudioUnit::SetCurrentDevice(std::string deviceID){
	UInt32 size=sizeof(AudioDeviceID);
	AudioDeviceID inputDevice=NULL;
	OSStatus status;
	
	if(deviceID=="default"){
		AudioObjectPropertyAddress propertyAddress;
		propertyAddress.mSelector = kAudioHardwarePropertyDefaultOutputDevice;
		propertyAddress.mScope = kAudioObjectPropertyScopeGlobal;
		propertyAddress.mElement = kAudioObjectPropertyElementMaster;
		UInt32 propsize = sizeof(AudioDeviceID);
		status = AudioObjectGetPropertyData(kAudioObjectSystemObject, &propertyAddress, 0, NULL, &propsize, &inputDevice);
		CHECK_AU_ERROR(status, "Error getting default input device");
	}else{
		AudioObjectPropertyAddress propertyAddress = {
			kAudioHardwarePropertyDevices,
			kAudioObjectPropertyScopeGlobal,
			kAudioObjectPropertyElementMaster
		};
		UInt32 dataSize = 0;
		status = AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &propertyAddress, 0, NULL, &dataSize);
		CHECK_AU_ERROR(status, "Error getting devices size");
		UInt32 deviceCount = (UInt32)(dataSize / sizeof(AudioDeviceID));
		AudioDeviceID audioDevices[deviceCount];
		status = AudioObjectGetPropertyData(kAudioObjectSystemObject, &propertyAddress, 0, NULL, &dataSize, audioDevices);
		CHECK_AU_ERROR(status, "Error getting device list");
		for(UInt32 i = 0; i < deviceCount; ++i) {
			// Query device UID
			CFStringRef deviceUID = NULL;
			dataSize = sizeof(deviceUID);
			propertyAddress.mSelector = kAudioDevicePropertyDeviceUID;
			status = AudioObjectGetPropertyData(audioDevices[i], &propertyAddress, 0, NULL, &dataSize, &deviceUID);
			CHECK_AU_ERROR(status, "Error getting device uid");
			char buf[1024];
			CFStringGetCString(deviceUID, buf, 1024, kCFStringEncodingUTF8);
			if(deviceID==buf){
				LOGV("Found device for id %s", buf);
				inputDevice=audioDevices[i];
				break;
			}
		}
		if(!inputDevice){
			LOGW("Requested device not found, using default");
			SetCurrentDevice("default");
			return;
		}
	}
 
	status =AudioUnitSetProperty(unit,
							  kAudioOutputUnitProperty_CurrentDevice,
							  kAudioUnitScope_Global,
							  kOutputBus,
							  &inputDevice,
							  size);
	CHECK_AU_ERROR(status, "Error setting output device");
	
	AudioStreamBasicDescription hardwareFormat;
	size=sizeof(hardwareFormat);
	status=AudioUnitGetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, kOutputBus, &hardwareFormat, &size);
	CHECK_AU_ERROR(status, "Error getting hardware format");
	hardwareSampleRate=hardwareFormat.mSampleRate;
	
	AudioStreamBasicDescription desiredFormat={
		.mSampleRate=48000, .mFormatID=kAudioFormatLinearPCM, .mFormatFlags=kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked | kAudioFormatFlagsNativeEndian,
		.mFramesPerPacket=1, .mChannelsPerFrame=1, .mBitsPerChannel=16, .mBytesPerPacket=2, .mBytesPerFrame=2
	};
	
	status=AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, kOutputBus, &desiredFormat, sizeof(desiredFormat));
	CHECK_AU_ERROR(status, "Error setting format");
	
	LOGD("Switched playback device, new sample rate %d", hardwareSampleRate);
	
	this->currentDevice=deviceID;
}

OSStatus AudioOutputAudioUnit::DefaultDeviceChangedCallback(AudioObjectID inObjectID, UInt32 inNumberAddresses, const AudioObjectPropertyAddress *inAddresses, void *inClientData){
	LOGV("System default input device changed");
	AudioOutputAudioUnit* self=(AudioOutputAudioUnit*)inClientData;
	if(self->currentDevice=="default"){
		self->SetCurrentDevice(self->currentDevice);
	}
	return noErr;
}