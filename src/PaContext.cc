/* Copyright 2019 Streampunk Media Ltd.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
*/

#include "PaContext.h"
#include "Params.h"
#include "Chunks.h"
#include <portaudio.h>
#include <thread>

namespace streampunk {

double timeDeltaMs = 0.0;
double prevTime = 0.0;

int PaCallback(const void *input, void *output, unsigned long frameCount,
               const PaStreamCallbackTimeInfo *timeInfo,
               PaStreamCallbackFlags statusFlags, void *userData) {
  PaContext *paContext = (PaContext *)userData;
  double inTimestamp = timeInfo->inputBufferAdcTime > 0.0 ?
    timeInfo->inputBufferAdcTime :
    paContext->getCurTime() - paContext->getInLatency(); // approximation for timestamp of first sample

  paContext->checkStatus(statusFlags);

  double currentTime = paContext->getCurTime();
  double samplesMs = frameCount / 1.0 / 48.0;
  double actualMs = (currentTime - prevTime) * 1000.0;
  double msToSkip = 0;
  // We cannot rely on the underflow flag--sometimes it is not triggered
  // correctly :(
  if (timeDeltaMs > samplesMs * 3) {
    // We underflowed! Let's skip all of the milliseconds we were waiting for
    // more data (minus the ms we're buffering this loop iteration).
    msToSkip = timeDeltaMs - samplesMs;
    timeDeltaMs -= msToSkip;
  }

  timeDeltaMs += actualMs - samplesMs;
  prevTime = currentTime;

  // printf("PaCallback output %p, frameCount %d\n", output, frameCount);
  int inRetCode = paContext->hasInput() && paContext->readPaBuffer(input, frameCount, inTimestamp) ? paContinue : paComplete;
  int outRetCode = paContext->hasOutput() && paContext->fillPaBuffer(output, frameCount, msToSkip) ? paContinue : paComplete;
  return ((inRetCode == paComplete) && (outRetCode == paComplete)) ? paComplete : paContinue;
}

PaContext::PaContext(napi_env env, napi_value inOptions, napi_value outOptions)
  : mInOptions(checkOptions(env, inOptions) ? std::make_shared<AudioOptions>(env, inOptions) : std::shared_ptr<AudioOptions>()),
    mOutOptions(checkOptions(env, outOptions) ? std::make_shared<AudioOptions>(env, outOptions) : std::shared_ptr<AudioOptions>()),
    mInChunks(new Chunks(mInOptions ? mInOptions->maxQueue() : 0)),
    mOutChunks(new Chunks(mOutOptions ? mOutOptions->maxQueue() : 0)),
    mStream(nullptr) {

  PaError errCode = Pa_Initialize();
  if (errCode != paNoError) {
    std::string err = std::string("Could not initialize PortAudio: ") + Pa_GetErrorText(errCode);
    napi_throw_error(env, nullptr, err.c_str());
    return;
  }

  if (!mInOptions && !mOutOptions) {
    napi_throw_error(env, nullptr, "Input and/or Output options must be specified");
    return;
  }

  if (mInOptions && mOutOptions &&
      (mInOptions->sampleRate() != mOutOptions->sampleRate())) {
    napi_throw_error(env, nullptr, "Input and Output sample rates must match");
    return;
  }

  printf("%s\n", Pa_GetVersionInfo()->versionText);
  if (mInOptions)
    printf("Input %s\n", mInOptions->toString().c_str());
  if (mOutOptions)
    printf("Output %s\n", mOutOptions->toString().c_str());

  double sampleRate;
  PaStreamParameters inParams;
  memset(&inParams, 0, sizeof(PaStreamParameters));
  if (mInOptions)
    setParams(env, /*isInput*/true, mInOptions, inParams, sampleRate);

  PaStreamParameters outParams;
  memset(&outParams, 0, sizeof(PaStreamParameters));
  if (mOutOptions)
    setParams(env, /*isInput*/false, mOutOptions, outParams, sampleRate);

  uint32_t framesPerBuffer = paFramesPerBufferUnspecified;
  #ifdef __arm__
  framesPerBuffer = 256;
  #endif
  uint32_t inFramesPerBuffer = mInOptions ? mInOptions->framesPerBuffer() : 0;
  uint32_t outFramesPerBuffer = mOutOptions ? mOutOptions->framesPerBuffer() : 0;
  if (!((0 == inFramesPerBuffer) && (0 == outFramesPerBuffer)))
    framesPerBuffer = std::max<uint32_t>(inFramesPerBuffer, outFramesPerBuffer);

  errCode = Pa_IsFormatSupported(mInOptions ? &inParams : NULL, mOutOptions ? &outParams : NULL, sampleRate);
  if (errCode != paFormatIsSupported) {
    std::string err = std::string("Format not supported: ") + Pa_GetErrorText(errCode);
    napi_throw_error(env, nullptr, err.c_str());
    return;
  }

  errCode = Pa_OpenStream(&mStream,
                          mInOptions ? &inParams : NULL,
                          mOutOptions ? &outParams : NULL,
                          sampleRate, framesPerBuffer,
                          paNoFlag, PaCallback, this);
  if (errCode != paNoError) {
    std::string err = std::string("Could not open stream: ") + Pa_GetErrorText(errCode);
    napi_throw_error(env, nullptr, err.c_str());
    return;
  }

  const PaStreamInfo *streamInfo = Pa_GetStreamInfo(mStream);
  mInLatency = streamInfo->inputLatency;
}

void PaContext::start(napi_env env) {
  // TODO(gnewman): Get this value from a parameter instead when we
  // instantiate the stream so we can correct for however long it takes for
  // Cedar to tell us to begin playback before we actually do
  prevTime = this->getCurTime();
  PaError errCode = Pa_StartStream(mStream);
  if (errCode != paNoError) {
    std::string err = std::string("Could not start stream: ") + Pa_GetErrorText(errCode);
    napi_throw_error(env, nullptr, err.c_str());
    return;
  }
}

void PaContext::stop(eStopFlag flag) {
  if (eStopFlag::ABORT == flag)
    Pa_AbortStream(mStream);
  else
    Pa_StopStream(mStream);
  Pa_CloseStream(mStream);
  Pa_Terminate();
}

std::shared_ptr<Chunk> PaContext::pullInChunk(uint32_t numBytes, bool &finished) {
  std::shared_ptr<Memory> result = Memory::makeNew(numBytes);
  finished = false;
  double timeStamp = 0.0;
  uint32_t bytesRead = fillBuffer(result->buf(), numBytes, timeStamp, mInChunks, finished, /*isInput*/true, 0);
  if (bytesRead != numBytes) {
    if (0 == bytesRead)
      result = std::shared_ptr<Memory>();
    else {
      std::shared_ptr<Memory> trimResult = Memory::makeNew(bytesRead);
      memcpy(trimResult->buf(), result->buf(), bytesRead);
      result = trimResult;
    }
  }

  return std::make_shared<Chunk>(result, timeStamp);
}

void PaContext::pushOutChunk(std::shared_ptr<Chunk> chunk) {
  mOutChunks->push(chunk);
}

std::string PaContext::checkStatus(uint32_t statusFlags) {
  if (statusFlags) {
    std::string err = std::string("portAudio status - ");
    if (statusFlags & paInputUnderflow)
      err += "input underflow ";
    if (statusFlags & paInputOverflow)
      err += "input overflow ";
    if (statusFlags & paOutputUnderflow)
      err += "output underflow ";
    if (statusFlags & paOutputOverflow)
      err += "output overflow ";
    if (statusFlags & paPrimingOutput)
      err += "priming output ";

    std::lock_guard<std::mutex> lk(m);
    mErrStr = err;
    return err;
  }
  return std::string("nada");
}

bool PaContext::getErrStr(std::string& errStr, bool isInput) {
  std::lock_guard<std::mutex> lk(m);
  std::shared_ptr<AudioOptions> options = isInput ? mInOptions : mOutOptions;
  if (options->closeOnError()) // propagate the error back to the stream handler
    errStr = mErrStr;
  else if (mErrStr.length())
    printf("AudioIO: %s\n", mErrStr.c_str());
  mErrStr.clear();
  return !errStr.empty();
}

void PaContext::quit() {
  if (mInOptions)
    mInChunks->quit();
  if (mOutOptions) {
    mOutChunks->quit();
    mOutChunks->waitDone();
  }
  // wait for next PaCallback to run
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
}

bool PaContext::readPaBuffer(const void *srcBuf, uint32_t frameCount, double inTimestamp) {
  uint32_t bytesAvailable = frameCount * mInOptions->channelCount() * mInOptions->sampleBits() / 8;
  std::shared_ptr<Memory> chunk = Memory::makeNew(bytesAvailable);
  memcpy(chunk->buf(), srcBuf, bytesAvailable);
  mInChunks->push(std::make_shared<Chunk>(chunk, inTimestamp));
  return true;
}

bool PaContext::fillPaBuffer(void *dstBuf, uint32_t frameCount, double msToSkip) {
  uint32_t bytesRemaining = frameCount * mOutOptions->channelCount() * mOutOptions->sampleBits() / 8;
  bool finished = false;
  double timeStamp = 0.0;
  uint32_t samplesToSkip = msToSkip * 48 ;
  uint32_t bytesToSkip = samplesToSkip * mOutOptions->sampleBits()  / 8 * mOutOptions->channelCount();

  fillBuffer((uint8_t *)dstBuf, bytesRemaining, timeStamp, mOutChunks, finished, /*isInput*/false, bytesToSkip);
  return !finished;
}

double PaContext::getCurTime() const  {
  return Pa_GetStreamTime(mStream);
}

// private
uint32_t PaContext::fillBuffer(uint8_t *buf, uint32_t numBytes, double &timeStamp,
                               std::shared_ptr<Chunks> chunks,
                               bool &finished, bool isInput, uint32_t bytesToSkip) {
  uint32_t bufOff = 0;
  timeStamp = 0.0;
  while (numBytes || bytesToSkip) {
    // Fetch the next chunk of source data if we need to; finish if no more chunks
    if (!chunks->curBuf() || (chunks->curBuf() && (chunks->curBytes() == chunks->curOffset()))) {
      // NOTE(gnewman): Underflow could happen here if we have to wait too long
      // for the next chunk.
      chunks->waitNext();
      if (!chunks->curBuf()) {
        printf("Finishing %s - %d bytes not available to fill the last buffer\n", isInput ? "input" : "output", numBytes);
        // Set output buffer to zeroes
        memset(buf + bufOff, 0, numBytes);
        finished = true;
        break;
      }
    }

    if (bytesToSkip) {
      uint32_t actualBytesToSkip = std::min<uint32_t>(bytesToSkip, chunks->curBytes() - chunks->curOffset());
      bytesToSkip -= actualBytesToSkip;
      // Increase our source buffer offset
      chunks->incOffset(actualBytesToSkip);
    } else {
      // Write the source to destination
      uint32_t bytesToWrite = std::min<uint32_t>(numBytes, chunks->curBytes() - chunks->curOffset());
      void *srcBuf = chunks->curBuf() + chunks->curOffset();
      memcpy(buf + bufOff, srcBuf, bytesToWrite);

      // Increase our source buffer offset
      chunks->incOffset(bytesToWrite);

      // Increase our destination buffer offset
      bufOff += bytesToWrite;
      numBytes -= bytesToWrite;
    }
  }

  return bufOff;
}

void PaContext::setParams(napi_env env, bool isInput,
                          std::shared_ptr<AudioOptions> options,
                          PaStreamParameters &params, double &sampleRate) {
  int32_t deviceID = (int32_t)options->deviceID();
  if ((deviceID >= 0) && (deviceID < Pa_GetDeviceCount()))
    params.device = (PaDeviceIndex)deviceID;
  else
    params.device = isInput ? Pa_GetDefaultInputDevice() : Pa_GetDefaultOutputDevice();
  if (params.device == paNoDevice) {
    napi_throw_error(env, nullptr, "No default device");
    return;
  }

  printf("%s device name is %s\n", isInput?"Input":"Output", Pa_GetDeviceInfo(params.device)->name);

  params.channelCount = options->channelCount();
  int maxChannels = isInput ? Pa_GetDeviceInfo(params.device)->maxInputChannels : Pa_GetDeviceInfo(params.device)->maxOutputChannels;
  if (params.channelCount > maxChannels) {
    napi_throw_error(env, nullptr, "Channel count exceeds maximum number of channels for device");
    return;
  }

  uint32_t sampleFormat = options->sampleFormat();
  switch(sampleFormat) {
  case 1: params.sampleFormat = paFloat32; break;
  case 8: params.sampleFormat = paInt8; break;
  case 16: params.sampleFormat = paInt16; break;
  case 24: params.sampleFormat = paInt24; break;
  case 32: params.sampleFormat = paInt32; break;
  default: {
      napi_throw_error(env, nullptr, "Invalid sampleFormat");
      return;
    }
  }

  params.suggestedLatency = isInput ? Pa_GetDeviceInfo(params.device)->defaultLowInputLatency :
                                      Pa_GetDeviceInfo(params.device)->defaultLowOutputLatency;
  params.hostApiSpecificStreamInfo = NULL;

  sampleRate = (double)options->sampleRate();

  #ifdef __arm__
  params.suggestedLatency = isInput ? Pa_GetDeviceInfo(params.device)->defaultHighInputLatency :
                                      Pa_GetDeviceInfo(params.device)->defaultHighOutputLatency;
  #endif
}

} // namespace streampunk
